// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Kirin 9000C ("charlotte") HUF S UFS bring-up.
 *
 * The HUF S controller exposes the UniPro/MPHY attribute space as a plain
 * MMIO window (resource #2, dword indexed): each UIC attribute N lives at
 * base + (N << 2), and DME_LOCAL_OPC_STATE reports whether the access is
 * still busy.  There is no interrupt involvement, so a simple poll works.
 *
 * The ASIC MPHY must be configured before DME_LINKSTARTUP, otherwise the
 * standard UIC link startup times out.  The sequence below is the vendor
 * "v300" (ASIC) pre-link sequence from
 * platform_source/basicplatform/drivers/scsi/ufs/ufs_scorpio.c.
 *
 * Everything here only touches host/PHY registers, never the flash data.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include <ufs/ufshcd.h>
#include "ufshcd-pltfrm.h"

/* Standard UFSHCI registers */
#define UFSHCI_CAP		0x00
#define UFSHCI_VER		0x08
#define UFSHCI_HCS		0x30
#define UFSHCI_HCE		0x34
#define UFSHCI_IS		0x20

/* SoC blocks used by ufs_soc_init() (vendor ufs_scorpio.c) */
#define CHARLOTTE_SCTRL_BASE	0xFB21B000UL
#define CHARLOTTE_ACTRL_BASE	0xFB214000UL
#define SCTRL_SCPERRSTEN1	0x20C
#define SCTRL_SCPERRSTDIS1	0x210
#define SCTRL_SCPERRSTSTAT1	0x214
#define SCTRL_SCCLKDIV9		0x274
#define SCTRL_SCPEREN4		0x1B0
#define SCTRL_SCEFUSEDATA2	0x00C
#define BIT_IP_RST_UFS_SUBSYS	14
#define BIT_IP_RST_UFS_SUBSYS_CRG	19
#define BIT_GT_CLK_UFS_SUBSYS	14
#define ACTRL_ISO_EN_GROUP0_PERI	0x000
#define ACTRL_AO_MEM_CTRL2	0x814
#define ACTRL_BITMSK_NONSEC_CTRL1 0x400
#define UFS_SYSCTRL_RESET_CTRL	0x008
#define UFS_SYSCTRL_MEMCTRL_TP	0x280
#define UFS_SYSCTRL_AO_MEMCTRL	0x284
#define UFS_SYSCTRL_MEMCTRL	0x288
#define UFS_IB_MST_RAM_TMOD	0x104
#define UFS_MPHY_MCU_MEMCTRL	0x00C

/* HUF S UniPro attribute window */
#define HUFS_UNIPRO_SHIFT	2
#define DME_LOCAL_OPC_STATE	0xD066
#define LOCAL_ATTR_BUSY		0x9

/* Attributes used by the pre-link sequence */
#define DME_PWR_BUSY			0x8
#define PA_GRANULARITY		0x15aa	/* PA_TActivate sibling */
#define DME_MPHYCFGUPDT		0xD014
#define DME_MPHYCFGUPDT_EN	BIT(0)
#define MPHY_INIT		0xD6
#define MPHY_INIT_MASK		(BIT(0) | BIT(1))
#define MPHY_INIT_DONE		(BIT(0) | BIT(1))
#define DME_UNIPRO_STATE	0xD053
#define DME_LINKSTARTUPREQ	0xD008
#define MASK_DEBUG_UNIPRO_STATE	0x3F80
#define LINK_DOWN		(0x1 << 7)
#define LINK_UP			(0x2 << 7)
#define PA_PWRMODE		0x1571
#define DME_POWERMODEIND	0xD011

#define ATTR_MTX0(x)		((0x0 << 16) | (x))
#define ATTR_MTX1(x)		((0x1 << 16) | (x))
#define ATTR_MRX0(x)		((0x2 << 16) | (x))
#define ATTR_MRX1(x)		((0x3 << 16) | (x))

#define TX_AFE_CONFIG_III	0x7C
#define TX_VCM_VREF_237		0xF
#define RX_AFE_CTRL_IV		0x14
#define CDR_RESET_EN		BIT(2)
#define AFE_CONFIG_VIII		0x56
#define TX_TRAILING_CLOCKS_REG	0x1564
#define TX_TRAILING_CLK_NUMS	0xFA
#define VS_DDBUG_SAVE_CONFIG_TIME 0x9529
#define SAVECFG_250NS		0x4

#define TX_DMY1			0x0052
#define RX_R_CAL		0x00E7
#define TX_LEVEL		12
#define RX_LEVEL		9
#define R_MULTIPLIER		10
#define R_DIVISOR		9
#define HALF_R_DIVISOR		(R_DIVISOR / 2)
#define MIN_TX_R		0x1
#define MAX_TX_R		0xF
#define MIN_RX_R		0x1
#define MAX_RX_R		0x1F

struct ufs_attr_cfg {
	u32 addr;
	u32 val;
};

/* Vendor "v300" (ASIC) MPHY pre-link configuration */
static const struct ufs_attr_cfg hufs_mphy_v300_pre_link_attr[] = {
	{ 0x000015A8, 0x00000013 }, /* PA_TActivate */
	{ 0x0000156A, 0x00000002 }, /* PA_HSSeries */
	{ 0x00020009, 0x00000001 }, /* Rx SKP_DET_SEL, lane0 */
	{ 0x00030009, 0x00000001 }, /* Rx SKP_DET_SEL, lane1 */
	{ 0x000000DF, 0x00000002 }, /* VCO_AUTO_CHG */
	{ 0x00020003, 0x0000000A }, /* AD_DIF_P_LS_TIMEOUT_VAL, lane0 */
	{ 0x00030003, 0x0000000A }, /* AD_DIF_P_LS_TIMEOUT_VAL, lane1 */
	{ 0x00020004, 0x00000064 }, /* AD_DIF_N_LS_TIMEOUT_VAL, lane0 */
	{ 0x00030004, 0x00000064 }, /* AD_DIF_N_LS_TIMEOUT_VAL, lane1 */
	{ 0x00020017, 0x00000084 }, /* RX_AFE_CTRL lane0, HW calib */
	{ 0x00030017, 0x00000084 }, /* RX_AFE_CTRL lane1, HW calib */
	{ 0x00000057, 0x00000000 }, /* AFE_CONFIG_IX lane0 */
	{ 0x00010057, 0x00000000 }, /* AFE_CONFIG_IX lane1 */
	{ 0x00000059, 0x00000022 }, /* AFE_CONFIG_XII lane0 */
	{ 0x00010059, 0x00000022 }, /* AFE_CONFIG_XII lane1 */
	{ 0x0000007B, 0x00000023 }, /* TX_AFE_CONFIG_II lane0 */
	{ 0x0001007B, 0x00000023 }, /* TX_AFE_CONFIG_II lane1 */
	{ 0x000200F0, 0x00000001 }, /* RX enable lane0 */
	{ 0x000300F0, 0x00000001 }, /* RX enable lane1 */
	{ 0x000200C7, 0x00000001 }, /* enable override */
	{ 0x000300C7, 0x00000001 }, /* enable override */
	{ 0x00020092, 0x00000001 }, /* RX_HIBERN8TIME_CAPABILITY lane0 */
	{ 0x00030092, 0x00000001 }, /* RX_HIBERN8TIME_CAPABILITY lane1 */
	{ 0x0002009D, 0x00000001 }, /* RX_HS_Equalizer_Setting_Capability l0 */
	{ 0x0003009D, 0x00000001 }, /* RX_HS_Equalizer_Setting_Capability l1 */
	{ 0x000200C7, 0x00000000 }, /* disable override */
	{ 0x000300C7, 0x00000000 }, /* disable override */
	{ 0x000200E9, 0x00000040 }, /* RX_HS_DATA_VALID_TIMER_VAL0 */
	{ 0x000300E9, 0x00000040 }, /* RX_HS_DATA_VALID_TIMER_VAL0 */
	{ 0x000200EA, 0x0000001F }, /* RX_HS_DATA_VALID_TIMER_VAL1 */
	{ 0x000300EA, 0x0000001F }, /* RX_HS_DATA_VALID_TIMER_VAL1 */
	{ 0x000000C1, 0x00000003 }, /* TX_RG_PLL_CP */
	{ 0x000100C1, 0x00000003 }, /* TX_RG_PLL_CP */
	{ 0x000000D2, 0x0000000F }, /* TX_RG_PLL_LPF_R */
	{ 0x000100D2, 0x0000000F }, /* TX_RG_PLL_LPF_R */
};

struct charlotte_hufs_host {
	int core_irq[8];
	unsigned int nr_core_irq;
	unsigned int intr_seen;
	void __iomem *sctrl;
	void __iomem *actrl;
	void __iomem *sys_ctrl;
	void __iomem *mphy_mcu;
	void __iomem *ib_mst;
	void __iomem *unipro;
	struct clk *ref_clk;
	struct reset_control *rst;
};

static void charlotte_hufs_qos_init(struct ufs_hba *hba);

static int hufs_uic_wait_idle(struct charlotte_hufs_host *host)
{
	unsigned int retries = 1000;
	u32 state;

	do {
		state = readl(host->unipro +
			      (DME_LOCAL_OPC_STATE << HUFS_UNIPRO_SHIFT));
		if (state != LOCAL_ATTR_BUSY)
			return 0;
		udelay(10);
	} while (--retries);

	return -ETIMEDOUT;
}

static int hufs_uic_write(struct charlotte_hufs_host *host, u32 attr, u32 val)
{
	writel(val, host->unipro + (attr << HUFS_UNIPRO_SHIFT));
	return hufs_uic_wait_idle(host);
}

static int hufs_uic_read(struct charlotte_hufs_host *host, u32 attr, u32 *val)
{
	*val = readl(host->unipro + (attr << HUFS_UNIPRO_SHIFT));
	return hufs_uic_wait_idle(host);
}

static int hufs_wait_mphy_init(struct charlotte_hufs_host *host)
{
	unsigned int retries = 4000;	/* 40 ms */
	u32 val;

	do {
		if (hufs_uic_read(host, MPHY_INIT, &val))
			return -ETIMEDOUT;
		if ((val & MPHY_INIT_MASK) == MPHY_INIT_DONE)
			return 0;
		udelay(10);
	} while (--retries);

	return -ETIMEDOUT;
}

static void hufs_tx_rx_r_calc(struct charlotte_hufs_host *host)
{
	u32 val = 0;

	if (hufs_uic_read(host, ATTR_MTX0(TX_DMY1), &val))
		return;
	val &= MAX_TX_R;
	val = (val + TX_LEVEL) * R_MULTIPLIER;
	val = (val % R_DIVISOR) ? (val / R_DIVISOR + MIN_TX_R) :
				  (val / R_DIVISOR);
	val -= TX_LEVEL;
	if (val > MAX_TX_R)
		val = MAX_TX_R;
	hufs_uic_write(host, ATTR_MTX0(TX_DMY1), val);
	hufs_uic_write(host, ATTR_MTX1(TX_DMY1), val);
	hufs_uic_write(host, DME_MPHYCFGUPDT, DME_MPHYCFGUPDT_EN);

	if (hufs_uic_read(host, ATTR_MRX0(RX_R_CAL), &val))
		return;
	val &= MAX_RX_R;
	val = (val + RX_LEVEL) * R_MULTIPLIER;
	val = (val % R_DIVISOR > HALF_R_DIVISOR) ?
	      (val / R_DIVISOR + MIN_RX_R) : (val / R_DIVISOR);
	val -= RX_LEVEL;
	if (val > MAX_RX_R)
		val = MAX_RX_R;
	hufs_uic_write(host, ATTR_MRX0(RX_R_CAL), val);
	hufs_uic_write(host, ATTR_MRX1(RX_R_CAL), val);
}


/*
 * hufs_mphy_v300_post_link_attr - verbatim from the vendor driver
 * (ufs_scorpio.c).  These are UniPro N_/T_ (network/transport) connection
 * attributes plus a few MPHY knobs.  They must be programmed after the DME
 * link is up and before the first UPIU, otherwise the UTP transfer fails
 * with a controller fatal error even though DME_UNIPRO_STATE says LINK_UP.
 */
static const struct ufs_attr_cfg hufs_mphy_v300_post_link_attr[] = {
	{ 0x00003000, 0x00000000 },	/* N_DeviceID */
	{ 0x00003001, 0x00000001 },	/* N_DeviceID_valid */
	{ 0x00004020, 0x00000000 },	/* T_ConnectionState */
	{ 0x00004022, 0x00000000 },	/* T_PeerCPortID */
	{ 0x00004021, 0x00000001 },	/* T_PeerDeviceID */
	{ 0x00004023, 0x00000000 },	/* T_TrafficClass */
	{ 0x00004020, 0x00000001 },	/* T_ConnectionState */
	{ 0x0002000A, 0x00000002 },	/* RX H8_TIMEOUT_VAL, lane 0 */
	{ 0x0003000A, 0x00000002 },	/* RX H8_TIMEOUT_VAL, lane 1 */
	{ 0x000200EB, 0x00000064 },	/* AD_DIF_N_TIMEOUT_VAL */
	{ 0x000300EB, 0x00000064 },	/* AD_DIF_N_TIMEOUT_VAL */
	{ 0x0002000E, 0x000000F0 },	/* RX_H8_EXIT_TIME */
	{ 0x0003000E, 0x000000F0 },	/* RX_H8_EXIT_TIME */
	{ 0x000000DF, 0x00000003 },	/* VCO_AUTO_CHG */
	{ 0x000015A8, 0x00000005 },	/* PA_TActivate */
};

static void charlotte_hufs_phy_post_link(struct ufs_hba *hba,
					 struct charlotte_hufs_host *host)
{
	u32 val = 0;
	int i;

	dev_info(hba->dev, "charlotte hufs: MPHY post-link config\n");

	for (i = 0; i < ARRAY_SIZE(hufs_mphy_v300_post_link_attr); i++) {
		if (hufs_uic_write(host, hufs_mphy_v300_post_link_attr[i].addr,
				   hufs_mphy_v300_post_link_attr[i].val))
			dev_warn(hba->dev, "post uic write 0x%x failed\n",
				 hufs_mphy_v300_post_link_attr[i].addr);
	}

	/* vendor check_pa_granularity(): PA_GRANULARITY must be >= 6 */
	if (!hufs_uic_read(host, PA_GRANULARITY, &val) && val < 6)
		hufs_uic_write(host, PA_GRANULARITY, 6);

	dev_info(hba->dev, "charlotte hufs: MPHY post-link config done\n");
}

static void charlotte_hufs_phy_pre_link(struct ufs_hba *hba,
					struct charlotte_hufs_host *host)
{
	u32 val = 0;
	int i;

	dev_info(hba->dev, "charlotte hufs: MPHY pre-link config\n");

	for (i = 0; i < ARRAY_SIZE(hufs_mphy_v300_pre_link_attr); i++) {
		if (hufs_uic_write(host, hufs_mphy_v300_pre_link_attr[i].addr,
				   hufs_mphy_v300_pre_link_attr[i].val))
			dev_warn(hba->dev, "uic write 0x%x failed\n",
				 hufs_mphy_v300_pre_link_attr[i].addr);
	}

	/* TX VCM VREF = 237.5mV */
	hufs_uic_write(host, ATTR_MTX0(TX_AFE_CONFIG_III), TX_VCM_VREF_237);
	hufs_uic_write(host, ATTR_MTX1(TX_AFE_CONFIG_III), TX_VCM_VREF_237);

	/* CDR reset */
	if (!hufs_uic_read(host, ATTR_MRX0(RX_AFE_CTRL_IV), &val)) {
		val |= CDR_RESET_EN;
		hufs_uic_write(host, ATTR_MRX0(RX_AFE_CTRL_IV), val);
		hufs_uic_write(host, ATTR_MRX1(RX_AFE_CTRL_IV), val);
	}

	/* analog EQ slicer enable */
	if (!hufs_uic_read(host, ATTR_MTX0(AFE_CONFIG_VIII), &val)) {
		val |= BIT(0);
		hufs_uic_write(host, ATTR_MTX0(AFE_CONFIG_VIII), val);
		hufs_uic_write(host, ATTR_MTX1(AFE_CONFIG_VIII), val);
	}

	/* PA_TxTrailingClocks */
	hufs_uic_write(host, TX_TRAILING_CLOCKS_REG, TX_TRAILING_CLK_NUMS);

	/* save-config time */
	hufs_uic_write(host, VS_DDBUG_SAVE_CONFIG_TIME, SAVECFG_250NS);

	/* commit, wait for the MPHY MCU, then calibrate and commit again */
	hufs_uic_write(host, DME_MPHYCFGUPDT, DME_MPHYCFGUPDT_EN);
	if (hufs_wait_mphy_init(host))
		dev_warn(hba->dev, "charlotte hufs: MPHY init did not complete\n");
	else
		dev_info(hba->dev, "charlotte hufs: MPHY init done\n");

	hufs_tx_rx_r_calc(host);
	hufs_uic_write(host, DME_MPHYCFGUPDT, DME_MPHYCFGUPDT_EN);

	dev_info(hba->dev, "charlotte hufs: MPHY pre-link config done\n");
}

/*
 * Vendor ufs_soc_init(): bring the UFS subsystem clock/reset tree up.
 * Without this the controller never sets UCRDY, so both the standard UIC
 * command interface and the HUF S private UniPro path stay dead.
 */
static void charlotte_hufs_soc_init(struct ufs_hba *hba)
{
	struct charlotte_hufs_host *host = hba->priv;
	unsigned int retry;
	u32 val, r;

	dev_info(hba->dev, "charlotte hufs: soc init\n");

	/* 1. SD/DS = 0 */
	writel(0x00030000, host->actrl + ACTRL_AO_MEM_CTRL2);

	/* 2. release UFS subsystem CRG reset */
	val = readl(host->sctrl + SCTRL_SCPERRSTDIS1);
	writel(val | BIT(BIT_IP_RST_UFS_SUBSYS_CRG),
	       host->sctrl + SCTRL_SCPERRSTDIS1);
	for (retry = 2000; retry; retry--) {
		if (!(readl(host->sctrl + SCTRL_SCPERRSTSTAT1) &
		      BIT(BIT_IP_RST_UFS_SUBSYS_CRG)))
			break;
		udelay(1);
	}
	if (!retry)
		dev_warn(hba->dev, "charlotte hufs: subsys CRG reset timeout\n");

	/* 3. reset the UFS subsystem */
	val = readl(host->sctrl + SCTRL_SCPERRSTEN1);
	writel(val | BIT(BIT_IP_RST_UFS_SUBSYS),
	       host->sctrl + SCTRL_SCPERRSTEN1);

	/* 4. UFS subsystem clock = 224 MHz */
	writel(0x003F0006, host->sctrl + SCTRL_SCCLKDIV9);

	/* 5. release MPHY isolation */
	writel(0x00210000, host->actrl + ACTRL_ISO_EN_GROUP0_PERI);

	/* 6. enable clk_ufs_subsys */
	val = readl(host->sctrl + SCTRL_SCPEREN4);
	writel(val | BIT(BIT_GT_CLK_UFS_SUBSYS), host->sctrl + SCTRL_SCPEREN4);

	/* 7. device hardware reset (bit10 rst_device_rst_n, bit26 mask) */
	writel(0x04000000, host->actrl + ACTRL_BITMSK_NONSEC_CTRL1);
	mdelay(2);
	writel(0x04000400, host->actrl + ACTRL_BITMSK_NONSEC_CTRL1);
	mdelay(10);

	/* 8. release the master reset */
	val = readl(host->sctrl + SCTRL_SCPERRSTDIS1);
	writel(val | BIT(BIT_IP_RST_UFS_SUBSYS),
	       host->sctrl + SCTRL_SCPERRSTDIS1);

	/* 9. release internal resets */
	writel(0xFFFFFFFF, host->sys_ctrl + UFS_SYSCTRL_RESET_CTRL);

	/* 10. memory TRA configuration */
	val = readl(host->sctrl + SCTRL_SCEFUSEDATA2);
	if (!(val & BIT(20))) {
		r = readl(host->ib_mst + UFS_IB_MST_RAM_TMOD);
		r |= BIT(11);
		r &= ~BIT(12);
		writel(r, host->ib_mst + UFS_IB_MST_RAM_TMOD);

		r = readl(host->sys_ctrl + UFS_SYSCTRL_MEMCTRL_TP);
		r |= BIT(11);
		r &= ~BIT(12);
		writel(r, host->sys_ctrl + UFS_SYSCTRL_MEMCTRL_TP);
	}
	if (!(val & BIT(15))) {
		r = readl(host->sys_ctrl + UFS_SYSCTRL_MEMCTRL);
		r |= BIT(16);
		r &= ~BIT(17);
		writel(r, host->sys_ctrl + UFS_SYSCTRL_MEMCTRL);

		r = readl(host->sys_ctrl + UFS_SYSCTRL_AO_MEMCTRL);
		r |= BIT(16);
		r &= ~BIT(17);
		writel(r, host->sys_ctrl + UFS_SYSCTRL_AO_MEMCTRL);

		r = readl(host->mphy_mcu + UFS_MPHY_MCU_MEMCTRL);
		r |= BIT(16);
		r &= ~BIT(17);
		writel(r, host->mphy_mcu + UFS_MPHY_MCU_MEMCTRL);
	}

	mdelay(1);
}

static irqreturn_t charlotte_hufs_core_intr(int irq, void *data)
{
	struct ufs_hba *hba = data;
	struct charlotte_hufs_host *host = hba->priv;
	unsigned int core;

	if (!host->nr_core_irq)
		return IRQ_NONE;
	core = irq - host->core_irq[0];
	if (host->intr_seen < 4) {
		dev_info(hba->dev, "charlotte hufs: core%u irq\n", core);
		host->intr_seen++;
	}
	if (ufshcd_charlotte_core_compl(hba, core))
		return IRQ_HANDLED;
	return IRQ_NONE;
}

static int charlotte_hufs_init(struct ufs_hba *hba)
{
	struct platform_device *pdev = to_platform_device(hba->dev);
	struct device *dev = hba->dev;
	struct charlotte_hufs_host *host;
	int i, ret;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	hba->priv = host;

	/*
	 * Resource 2 is the HUF S UniPro/MPHY attribute window; bank 0 is
	 * already mapped by ufshcd_pltfrm_init() (use hba->mmio_base there).
	 */
	host->unipro = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(host->unipro))
		return dev_err_probe(dev, PTR_ERR(host->unipro),
				     "failed to map HUF S UniPro window\n");

	dev_info(dev, "UFSHCI CAP=%#x VER=%#x HCS=%#x\n",
		 readl(hba->mmio_base + UFSHCI_CAP),
		 readl(hba->mmio_base + UFSHCI_VER),
		 readl(hba->mmio_base + UFSHCI_HCS));

	host->sys_ctrl = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(host->sys_ctrl))
		return dev_err_probe(dev, PTR_ERR(host->sys_ctrl),
				     "failed to map UFS sys ctrl\n");

	host->mphy_mcu = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(host->mphy_mcu))
		return dev_err_probe(dev, PTR_ERR(host->mphy_mcu),
				     "failed to map UFS MPHY MCU\n");

	host->ib_mst = devm_platform_ioremap_resource(pdev, 4);
	if (IS_ERR(host->ib_mst))
		return dev_err_probe(dev, PTR_ERR(host->ib_mst),
				     "failed to map UFS IB master\n");

	host->sctrl = devm_ioremap(dev, CHARLOTTE_SCTRL_BASE, 0x1000);
	if (!host->sctrl)
		return -ENOMEM;

	host->actrl = devm_ioremap(dev, CHARLOTTE_ACTRL_BASE, 0x1000);
	if (!host->actrl)
		return -ENOMEM;

	host->ref_clk = devm_clk_get_optional(dev, "clk_ufsio_ref");
	if (IS_ERR(host->ref_clk))
		return dev_err_probe(dev, PTR_ERR(host->ref_clk),
				     "failed to get clk_ufsio_ref\n");

	host->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(host->rst))
		return dev_err_probe(dev, PTR_ERR(host->rst),
				     "failed to get UFS reset\n");

	/*
	 * Do not use UFSHCI_QUIRK_BROKEN_HCE: the generic DME_RESET/DME_ENABLE
	 * primitives go through the standard UIC command register, which never
	 * completes on this controller.  Let the generic core enable HCE
	 * normally and take over the link startup ourselves (see
	 * charlotte_hufs_dme_link_startup).
	 */
	dev_info(dev, "charlotte hufs: normal HCE path + private link startup\n");

	/*
	 * HUF S uses per-core UTP queues: doorbell/IS/IE are repeated every
	 * 0x80 bytes starting at 0x2000, and each core has its own IRQ.
	 */
	hba->core_utr = true;
	for (i = 0; i < 8; i++) {
		int irq = platform_get_irq_optional(pdev, i + 1);

		if (irq < 0)
			break;
		ret = devm_request_irq(dev, irq, charlotte_hufs_core_intr,
				       IRQF_SHARED, "ufs-core", hba);
		if (ret)
			return ret;
		host->core_irq[i] = irq;
		host->nr_core_irq++;
	}
	dev_info(dev, "charlotte hufs: %u per-core irqs\n", host->nr_core_irq);

	charlotte_hufs_soc_init(hba);

	dev_info(dev, "charlotte hufs: after soc init CAP=%#x VER=%#x HCS=%#x HCE=%#x\n",
		 readl(hba->mmio_base + UFSHCI_CAP),
		 readl(hba->mmio_base + UFSHCI_VER),
		 readl(hba->mmio_base + UFSHCI_HCS),
		 readl(hba->mmio_base + UFSHCI_HCE));

	return 0;
}

/* Issue a UIC command through the standard register and poll IS for completion */
static int charlotte_hufs_hce_enable_notify(struct ufs_hba *hba,
					    enum ufs_notify_change_status status)
{
	if (status != POST_CHANGE)
		return 0;

	dev_info(hba->dev, "charlotte hufs: HCE=%#x HCS=%#x after enable\n",
		 readl(hba->mmio_base + UFSHCI_HCE),
		 readl(hba->mmio_base + UFSHCI_HCS));
	return 0;
}

static int charlotte_hufs_dme_link_startup(struct ufs_hba *hba)
{
	struct charlotte_hufs_host *host = hba->priv;
	unsigned int retries = 500;
	u32 val = 0;
	int ret;

	ret = hufs_uic_read(host, DME_UNIPRO_STATE, &val);
	if (ret)
		return ret;
	dev_info(hba->dev, "charlotte hufs: unipro state %#x before link startup\n",
		 val & MASK_DEBUG_UNIPRO_STATE);

	ret = hufs_uic_write(host, DME_LINKSTARTUPREQ, 0x1);
	if (ret)
		return ret;

	do {
		if (hufs_uic_read(host, DME_UNIPRO_STATE, &val))
			return -ETIMEDOUT;
		if ((val & MASK_DEBUG_UNIPRO_STATE) == LINK_UP) {
			dev_info(hba->dev, "charlotte hufs: link up (state %#x)\n",
				 val & MASK_DEBUG_UNIPRO_STATE);
			return 0;
		}
		usleep_range(1000, 2000);
	} while (--retries);

	dev_err(hba->dev, "charlotte hufs: link startup timeout, state %#x\n",
		val & MASK_DEBUG_UNIPRO_STATE);
	return -ETIMEDOUT;
}

static int charlotte_hufs_uic_change_pwr_mode(struct ufs_hba *hba, u8 mode)
{
	struct charlotte_hufs_host *host = hba->priv;
	u32 val = 0;
	int ret, try_, i;

	for (try_ = 0; try_ < 3; try_++) {
		ret = hufs_uic_write(host, PA_PWRMODE, mode);
		if (ret)
			return ret;

		for (i = 0; i < 200; i++) {
			if (hufs_uic_read(host, DME_POWERMODEIND, &val))
				return -ETIMEDOUT;
			if (val != 0xffffffff && !(val & DME_PWR_BUSY))
				break;
			usleep_range(500, 1000);
		}
		if (val != 0xffffffff && !(val & DME_PWR_BUSY))
			break;
	}

	dev_info(hba->dev,
		 "charlotte hufs: PA_PWRMODE %#x -> DME_POWERMODEIND %#x (tries %d)\n",
		 mode, val, try_ + 1);

	if (val == 0xffffffff || (val & DME_PWR_BUSY))
		return -ETIMEDOUT;

	return 0;
}

/*
 * The peer DME path (standard UIC commands) does not work on this
 * controller, so report the negotiated parameters the vendor driver ends up
 * with on this board: HS-G4, two lanes, rate B.  Matches the vendor DT
 * properties "hufs-use-HS-GEAR4" / "hufs-use-two-line".
 */
static int charlotte_hufs_get_max_pwr_mode(struct ufs_hba *hba)
{
	struct ufs_pa_layer_attr *p = &hba->max_pwr_info.info;

	p->pwr_tx = FAST_MODE;
	p->pwr_rx = FAST_MODE;
	p->hs_rate = PA_HS_MODE_B;
	p->lane_tx = 2;
	p->lane_rx = 2;
	p->gear_tx = UFS_HS_G4;
	p->gear_rx = UFS_HS_G4;
	hba->max_pwr_info.is_valid = true;

	dev_info(hba->dev,
		 "charlotte hufs: max pwr mode HS-G%d, %u lanes, rate %c\n",
		 p->gear_rx, p->lane_rx, p->hs_rate ? 'B' : 'A');

	return 0;
}

/*
 * Local DME access through the private UniPro window (0xfa400000).
 *
 * The core encodes UIC arguments as (mib << 16) | selector, while the HUF S
 * window is indexed with (selector << 16) | mib (the window is 1 MiB because
 * the selector selects one of four 256 KiB banks).  Only local attributes
 * (selector 0) are routed here, so the conversion is just the MIB.
 */
static int charlotte_hufs_dme_get(struct ufs_hba *hba, u32 attr_sel,
				  u32 *mib_val)
{
	struct charlotte_hufs_host *host = hba->priv;
	u32 mib = UIC_GET_ATTR_ID(attr_sel);

	if (!host || !host->unipro || !mib)
		return -EINVAL;

	return hufs_uic_read(host, mib, mib_val);
}

static int charlotte_hufs_dme_set(struct ufs_hba *hba, u32 attr_sel, u32 val)
{
	struct charlotte_hufs_host *host = hba->priv;
	u32 mib = UIC_GET_ATTR_ID(attr_sel);

	if (!host || !host->unipro || !mib)
		return -EINVAL;

	return hufs_uic_write(host, mib, val);
}

/*
 * Core negotiates the HS power mode now that DME reads work.  The MPHY /
 * PA layer needs its post-link knobs (PA_TActivate, PA_GRANULARITY, ...)
 * re-applied for the new gear afterwards.
 */
static int charlotte_hufs_pwr_change_notify(struct ufs_hba *hba,
		enum ufs_notify_change_status status,
		const struct ufs_pa_layer_attr *desired,
		struct ufs_pa_layer_attr *final)
{
	struct charlotte_hufs_host *host = hba->priv;
	u32 val = 0;

	if (status == PRE_CHANGE) {
		if (desired && final)
			*final = *desired;
		return 0;
	}

	/* POST_CHANGE: re-apply PA_TActivate / PA_GRANULARITY for the gear */
	charlotte_hufs_phy_post_link(hba, host);

	if (!hufs_uic_read(host, DME_POWERMODEIND, &val))
		dev_info(hba->dev,
			 "charlotte hufs: pwr change done: ind=%#x gear=%u lanes=%u pwr=%u hs_rate=%u\n",
			 val, hba->pwr_info.gear_rx, hba->pwr_info.lane_rx,
			 hba->pwr_info.pwr_rx, hba->pwr_info.hs_rate);

	return 0;
}

/*
 * In HUF S mode the standard UFSHCI error reporting is not meaningful:
 * the controller asserts bogus UIC_ERROR + CONTROLLER_FATAL_ERROR bits
 * during/after link startup, which makes the generic core declare a fatal
 * error and (worse) enter its error handler before the device exists.
 * Completions are delivered through the per-core queues instead, so keep
 * the main interrupt line quiet during bring-up.
 */
static void charlotte_hufs_quiesce_irq(struct ufs_hba *hba)
{
	writel(0, hba->mmio_base + 0x24);		/* IE  */
	writel(0xffffffff, hba->mmio_base + 0x20);	/* IS  */
	readl(hba->mmio_base + 0x38);			/* UECPA */
	readl(hba->mmio_base + 0x3c);			/* UECDL */
	readl(hba->mmio_base + 0x40);			/* UECN  */
	readl(hba->mmio_base + 0x44);			/* UECT  */
	readl(hba->mmio_base + 0x48);			/* UECDME */
	hba->saved_err = 0;
	hba->saved_uic_err = 0;
	hba->errors = 0;
}

/*
 * HUF S multi-queue arbitration defaults, taken from the working 5.10
 * driver/controller state (the subsystem reset in soc_init clears them).
 */
static void charlotte_hufs_qos_init(struct ufs_hba *hba)
{
	int i;

	writel(0x001C8012, hba->mmio_base + 0xc0);	/* PROC_MODE_CFG */
	writel(0x00003F1F, hba->mmio_base + 0xd4);	/* OUTSTANDING_NUM */
	writel(0x07070707, hba->mmio_base + 0xd8);
	writel(0x1F1F1F07, hba->mmio_base + 0xdc);
	writel(0x07070707, hba->mmio_base + 0xe0);
	writel(0x001F1F07, hba->mmio_base + 0xe4);
	writel(0x1D1D1D1D, hba->mmio_base + 0xe8);
	writel(0x000F0F1D, hba->mmio_base + 0xec);
	writel(0x00000044, hba->mmio_base + 0x3b0);

	for (i = 0; i < 8; i++)
		writel(0, hba->mmio_base + 0x2010 + i * 0x80);

	dev_info(hba->dev,
		 "charlotte hufs: qos init D4=%#x D8=%#x DC=%#x 3B0=%#x\n",
		 readl(hba->mmio_base + 0xd4),
		 readl(hba->mmio_base + 0xd8),
		 readl(hba->mmio_base + 0xdc),
		 readl(hba->mmio_base + 0x3b0));
}

static void charlotte_hufs_dump_utrd(struct ufs_hba *hba)
{
	u32 tag = hba->reserved_slot;
	u32 *p = (u32 *)hba->lrb[tag].utr_descriptor_ptr;

	dev_info(hba->dev,
		 "charlotte hufs: lrb[%u] utrd=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 tag, p[0], p[1], p[2], p[3], p[4], p[5],
		 p[6], p[7], p[8], p[9], p[10], p[11]);
}

static void charlotte_dump_range(struct ufs_hba *hba, const char *tag,
				 void __iomem *base, u32 len)
{
	u32 off;

	for (off = 0; off < len; off += 16) {
		dev_info(hba->dev, "D %s %04x: %08x %08x %08x %08x\n", tag, off,
			 readl(base + off), readl(base + off + 4),
			 readl(base + off + 8), readl(base + off + 12));
	}
}

/*
 * Mirror the per-core slot configuration observed on the working 5.10
 * system.  In particular 0x2020 reads 0xff there (all 8 slots usable);
 * after the subsystem reset it is 0, which leaves the per-core doorbell
 * set but the request is never fetched.
 */
static void charlotte_hufs_hw_config(struct ufs_hba *hba)
{
	int i;

	for (i = 0; i < 8; i++) {
		void __iomem *c = hba->mmio_base + 0x2000 + i * 0x80;

		writel(0, c + 0x10);	/* DOORBELL_QOS */
		writel(3, c + 0x0c);
		writel(0xff, c + 0x20);	/* slot enable / credit mask */
	}

	dev_info(hba->dev, "charlotte hufs: core0 cfg IS=%#x IE=%#x @c=%#x @20=%#x\n",
		 readl(hba->mmio_base + 0x2000), readl(hba->mmio_base + 0x2004),
		 readl(hba->mmio_base + 0x200c), readl(hba->mmio_base + 0x2020));
}

static void charlotte_hufs_dump_all(struct ufs_hba *hba)
{
	struct charlotte_hufs_host *host = hba->priv;

	charlotte_dump_range(hba, "hci", hba->mmio_base, 0x500);
	charlotte_dump_range(hba, "core", hba->mmio_base + 0x2000, 0x400);
	charlotte_dump_range(hba, "sysc", host->sys_ctrl, 0x100);
	charlotte_dump_range(hba, "mphy", host->mphy_mcu, 0x100);
	charlotte_dump_range(hba, "ibmst", host->ib_mst, 0x100);
	charlotte_dump_range(hba, "sctrl", host->sctrl, 0x500);
	charlotte_dump_range(hba, "actrl", host->actrl, 0x900);
	charlotte_dump_range(hba, "unip", host->unipro, 0x200);
}

static void charlotte_hufs_dump_core(struct ufs_hba *hba)
{
	int i;

	dev_info(hba->dev, "charlotte hufs: HCS=%#x ISg=%#x UTRLBA=%#x errs=%#x out=%#lx\n",
		 readl(hba->mmio_base + 0x30),
		 readl(hba->mmio_base + 0x20),
		 readl(hba->mmio_base + 0x50),
		 hba->errors, hba->outstanding_reqs);
	for (i = 0; i < 8; i++) {
		void __iomem *c = hba->mmio_base + 0x2000 + i * 0x80;

		dev_info(hba->dev,
			 "charlotte hufs: core%d IS=%#x IE=%#x DBR=%#x RSR=%#x\n",
			 i, readl(c), readl(c + 4), readl(c + 0x14), readl(c + 0x1c));
	}
}

static void charlotte_hufs_exit(struct ufs_hba *hba)
{
	charlotte_hufs_dump_core(hba);
	if (hba->utrdl_base_addr)
		charlotte_hufs_dump_utrd(hba);
}

static int charlotte_hufs_link_startup_notify(struct ufs_hba *hba,
					      enum ufs_notify_change_status status)
{
	struct charlotte_hufs_host *host = hba->priv;

	if (status == PRE_CHANGE) {
		charlotte_hufs_quiesce_irq(hba);
		charlotte_hufs_phy_pre_link(hba, host);
	} else {
		charlotte_hufs_quiesce_irq(hba);
		charlotte_hufs_phy_post_link(hba, host);
		charlotte_hufs_qos_init(hba);
		charlotte_hufs_hw_config(hba);
		ufshcd_charlotte_core_intr_enable(hba);
		/*
		 * Do not start the per-core queues here: UTRLBA is programmed
		 * later by the generic core (ufshcd_make_hba_operational) and
		 * enabling RSR against a zero base latches a bad state.
		 * ufshcd_enable_run_stop_reg() enables them at the right time.
		 */
		dev_info(hba->dev,
			 "charlotte hufs: dma utrdl=%pad ucdl=%pad\n",
			 &hba->utrdl_dma_addr, &hba->ucdl_dma_addr);
		charlotte_hufs_dump_core(hba);
		charlotte_hufs_dump_utrd(hba);
	}

	return 0;
}

/*
 * The controller only handles 32-bit DMA addresses (the working 5.10 kernel
 * places the UTRL/UCD buffers below 4 GiB).  Restrict coherent allocations
 * accordingly, otherwise the UTRD/UCD addresses end up above 4 GiB and the
 * controller cannot fetch them.
 */
static int charlotte_hufs_set_dma_mask(struct ufs_hba *hba)
{
	int ret;

	ret = dma_set_mask_and_coherent(hba->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_err(hba->dev, "charlotte hufs: 32-bit DMA mask failed %d\n",
			ret);
	return ret;
}

static const struct ufs_hba_variant_ops charlotte_hufs_vops = {
	.name			= "charlotte-hufs",
	.init			= charlotte_hufs_init,
	.exit			= charlotte_hufs_exit,
	.set_dma_mask		= charlotte_hufs_set_dma_mask,
	.hce_enable_notify	= charlotte_hufs_hce_enable_notify,
	.link_startup_notify	= charlotte_hufs_link_startup_notify,
	.dme_link_startup	= charlotte_hufs_dme_link_startup,
	.uic_change_pwr_mode	= charlotte_hufs_uic_change_pwr_mode,
	.dme_get		= charlotte_hufs_dme_get,
	.dme_set		= charlotte_hufs_dme_set,
	.get_max_pwr_mode	= charlotte_hufs_get_max_pwr_mode,
	.pwr_change_notify	= charlotte_hufs_pwr_change_notify,
};

static const struct of_device_id charlotte_hufs_of_match[] = {
	{ .compatible = "hisilicon,hufs", .data = &charlotte_hufs_vops },
	{ }
};
MODULE_DEVICE_TABLE(of, charlotte_hufs_of_match);

static int charlotte_hufs_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;

	match = of_match_node(charlotte_hufs_of_match, pdev->dev.of_node);
	if (!match)
		return -ENODEV;

	return ufshcd_pltfrm_init(pdev, match->data);
}

static void charlotte_hufs_remove(struct platform_device *pdev)
{
	ufshcd_pltfrm_remove(pdev);
}

static const struct dev_pm_ops charlotte_hufs_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufshcd_system_suspend, ufshcd_system_resume)
	SET_RUNTIME_PM_OPS(ufshcd_runtime_suspend, ufshcd_runtime_resume, NULL)
};

static struct platform_driver charlotte_hufs_driver = {
	.probe = charlotte_hufs_probe,
	.remove = charlotte_hufs_remove,
	.driver = {
		.name = "ufshcd-charlotte",
		.pm = &charlotte_hufs_pm_ops,
		.of_match_table = charlotte_hufs_of_match,
	},
};
module_platform_driver(charlotte_hufs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon charlotte HUF S UFS bring-up");
MODULE_ALIAS("platform:ufshcd-charlotte");
