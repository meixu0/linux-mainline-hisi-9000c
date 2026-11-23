// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kirin 9000C (charlotte) display-subsystem power bring-up probe.
 *
 * The vendor BSP powers the media1/vivobus/dss power domains through the
 * ATF "ip regulator" service:
 *
 *   arm_smccc_smc(make_fid(FID_REGU_GROUP=7, 0) = 0xC3000700,
 *                 regulator_id, REGULATOR_ENABLE(=1), 0, ...)
 *
 * with the AP vote bits set in the sctrl AOD register first:
 *   0xFB21B000 + 0x438:  bit0 AP media1, bit2 AP vivobus, bit4 AP dss
 *
 * Regulator ids (vendor char_autofs.h):
 *   0 media1_subsys, 1 media2_subsys, 2 vivobus, 3 vcodecsubsys,
 *   4 dsssubsys, 5 dispch1, 6 ipp, 7 vdec, ...
 *
 * This probe only powers things on and then tries to read the DPU register
 * window.  It does not program anything.
 */

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>

#define IP_REGULATOR_FID	0xC3000700UL
#define REGU_ENABLE		1

#define SCTRL_BASE		0xFB21B000UL
#define SCTRL_AOD_VOTE		0x438
#define VOTE_AP_MEDIA1		BIT(0)
#define VOTE_AP_VIVOBUS		BIT(2)
#define VOTE_AP_DSS		BIT(4)

static const struct {
	unsigned int id;
	const char *name;
} regus[] = {
	{ 0,  "media1_subsys" },
	{ 2,  "vivobus" },
	{ 4,  "dsssubsys" },
	{ 5,  "disp_ch1subsys" },
	{ 14, "vivobus_autodiv" },
};

static void smc_probe(unsigned long fid, unsigned long a1, unsigned long a2,
		      const char *what)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fid, a1, a2, 0, 0, 0, 0, 0, &res);
	pr_info("dss-probe: SMC %-22s fid=%#lx a1=%#lx -> %#lx %#lx %#lx %#lx\n",
		what, fid, a1, res.a0, res.a1, res.a2, res.a3);
}

static int __init dss_power_probe(void)
{
	void __iomem *sctrl, *crg, *dpu;
	unsigned int vote;
	int i;

	pr_info("dss-probe: start\n");

	/* 0. sanity: does the SMC conduit work at all? (PSCI version) */
	smc_probe(0x84000000UL, 0, 0, "PSCI_VERSION");

	/* 0b. our vendor FID with a bogus id, to see how ATF answers */
	smc_probe(IP_REGULATOR_FID, 0xff, 0, "regu bogus id");

	/* 1. AP votes for media1 / vivobus / dss */
	sctrl = ioremap(SCTRL_BASE, 0x1000);
	if (!sctrl) {
		pr_err("dss-probe: ioremap sctrl failed\n");
		return 0;
	}
	vote = readl(sctrl + SCTRL_AOD_VOTE);
	pr_info("dss-probe: aod vote before = %#x\n", vote);
	vote |= VOTE_AP_MEDIA1 | VOTE_AP_VIVOBUS | VOTE_AP_DSS;
	writel(vote, sctrl + SCTRL_AOD_VOTE);
	pr_info("dss-probe: aod vote after  = %#x\n",
		readl(sctrl + SCTRL_AOD_VOTE));

	/* 2. ask ATF to power the rails on */
	for (i = 0; i < ARRAY_SIZE(regus); i++)
		smc_probe(IP_REGULATOR_FID, regus[i].id, REGU_ENABLE,
			  regus[i].name);

	msleep(50);

	/* 2b. peek at the peripheral CRG block (always-on, vendor does too) */
	crg = ioremap(0xe8d01000, 0x1000);
	if (!crg) {
		pr_err("dss-probe: ioremap crg failed\n");
	} else {
		pr_info("dss-probe: CRG e8d01000: %08x %08x %08x %08x %08x %08x\n",
			readl(crg), readl(crg + 4), readl(crg + 8),
			readl(crg + 0xc), readl(crg + 0x10), readl(crg + 0x14));
		iounmap(crg);
	}

	/* 3. can we touch the DPU now? */
	dpu = ioremap(0xe8800000, 0x1000);
	if (!dpu) {
		pr_err("dss-probe: ioremap dpu failed\n");
	} else {
		pr_info("dss-probe: DPU readable: %08x %08x %08x %08x\n",
			readl(dpu), readl(dpu + 4), readl(dpu + 8),
			readl(dpu + 0x28));
		iounmap(dpu);
	}

	iounmap(sctrl);
	pr_info("dss-probe: end\n");
	return 0;
}
late_initcall(dss_power_probe);
