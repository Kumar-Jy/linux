// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include "drm/drm_bridge_connector.h"

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>

#include "msm_kms.h"
#include "dsi.h"
#include "drm/drm_notifier.h"
#include <linux/workqueue.h>

#define DSI_CLOCK_MASTER	DSI_0
#define DSI_CLOCK_SLAVE		DSI_1

#define DSI_LEFT		DSI_0
#define DSI_RIGHT		DSI_1

/* According to the current drm framework sequence, take the encoder of
 * DSI_1 as master encoder
 */
#define DSI_ENCODER_MASTER	DSI_1
#define DSI_ENCODER_SLAVE	DSI_0

struct msm_dsi_manager {
	struct msm_dsi *dsi[DSI_MAX];

	bool is_bonded_dsi;
	bool is_sync_needed;
	int master_dsi_link_id;
};

static struct msm_dsi_manager msm_dsim_glb;

#define IS_BONDED_DSI()		(msm_dsim_glb.is_bonded_dsi)
#define IS_SYNC_NEEDED()	(msm_dsim_glb.is_sync_needed)
#define IS_MASTER_DSI_LINK(id)	(msm_dsim_glb.master_dsi_link_id == id)

static inline struct msm_dsi *dsi_mgr_get_dsi(int id)
{
	return msm_dsim_glb.dsi[id];
}

static inline struct msm_dsi *dsi_mgr_get_other_dsi(int id)
{
	return msm_dsim_glb.dsi[(id + 1) % DSI_MAX];
}

static int dsi_mgr_parse_of(struct device_node *np, int id)
{
	struct msm_dsi_manager *msm_dsim = &msm_dsim_glb;

	/* We assume 2 dsi nodes have the same information of bonded dsi and
	 * sync-mode, and only one node specifies master in case of bonded mode.
	 */
	if (!msm_dsim->is_bonded_dsi)
		msm_dsim->is_bonded_dsi = of_property_read_bool(np, "qcom,dual-dsi-mode");

	if (msm_dsim->is_bonded_dsi) {
		if (of_property_read_bool(np, "qcom,master-dsi"))
			msm_dsim->master_dsi_link_id = id;
		if (!msm_dsim->is_sync_needed)
			msm_dsim->is_sync_needed = of_property_read_bool(
					np, "qcom,sync-dual-dsi");
	}

	return 0;
}

static int dsi_mgr_setup_components(int id)
{
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *other_dsi = dsi_mgr_get_other_dsi(id);
	struct msm_dsi *clk_master_dsi = dsi_mgr_get_dsi(DSI_CLOCK_MASTER);
	struct msm_dsi *clk_slave_dsi = dsi_mgr_get_dsi(DSI_CLOCK_SLAVE);
	int ret;

	if (!IS_BONDED_DSI()) {
		/*
		 * Set the usecase before calling msm_dsi_host_register(), which would
		 * already program the PLL source mux based on a default usecase.
		 */
		msm_dsi_phy_set_usecase(msm_dsi->phy, MSM_DSI_PHY_STANDALONE);
		msm_dsi_host_set_phy_mode(msm_dsi->host, msm_dsi->phy);

		ret = msm_dsi_host_register(msm_dsi->host);
		if (ret)
			return ret;
	} else if (other_dsi) {
		struct msm_dsi *master_link_dsi = IS_MASTER_DSI_LINK(id) ?
							msm_dsi : other_dsi;
		struct msm_dsi *slave_link_dsi = IS_MASTER_DSI_LINK(id) ?
							other_dsi : msm_dsi;

		/*
		 * PLL0 is to drive both DSI link clocks in bonded DSI mode.
		 *
		 * Set the usecase before calling msm_dsi_host_register(), which would
		 * already program the PLL source mux based on a default usecase.
		 */
		msm_dsi_phy_set_usecase(clk_master_dsi->phy,
					MSM_DSI_PHY_MASTER);
		msm_dsi_phy_set_usecase(clk_slave_dsi->phy,
					MSM_DSI_PHY_SLAVE);
		msm_dsi_host_set_phy_mode(msm_dsi->host, msm_dsi->phy);
		msm_dsi_host_set_phy_mode(other_dsi->host, other_dsi->phy);

		/* Register slave host first, so that slave DSI device
		 * has a chance to probe, and do not block the master
		 * DSI device's probe.
		 * Also, do not check defer for the slave host,
		 * because only master DSI device adds the panel to global
		 * panel list. The panel's device is the master DSI device.
		 */
		ret = msm_dsi_host_register(slave_link_dsi->host);
		if (ret)
			return ret;
		ret = msm_dsi_host_register(master_link_dsi->host);
		if (ret)
			return ret;
	}

	return 0;
}

static int enable_phy(struct msm_dsi *msm_dsi,
		      struct msm_dsi_phy_shared_timings *shared_timings)
{
	struct msm_dsi_phy_clk_request clk_req;
	bool is_bonded_dsi = IS_BONDED_DSI();

	msm_dsi_host_get_phy_clk_req(msm_dsi->host, &clk_req, is_bonded_dsi);

	return msm_dsi_phy_enable(msm_dsi->phy, &clk_req, shared_timings);
}

static int
dsi_mgr_phy_enable(int id,
		   struct msm_dsi_phy_shared_timings shared_timings[DSI_MAX])
{
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *mdsi = dsi_mgr_get_dsi(DSI_CLOCK_MASTER);
	struct msm_dsi *sdsi = dsi_mgr_get_dsi(DSI_CLOCK_SLAVE);
	int ret;

	/* In case of bonded DSI, some registers in PHY1 have been programmed
	 * during PLL0 clock's set_rate. The PHY1 reset called by host1 here
	 * will silently reset those PHY1 registers. Therefore we need to reset
	 * and enable both PHYs before any PLL clock operation.
	 */
	if (IS_BONDED_DSI() && mdsi && sdsi) {
		if (!mdsi->phy_enabled && !sdsi->phy_enabled) {
			msm_dsi_host_reset_phy(mdsi->host);
			msm_dsi_host_reset_phy(sdsi->host);

			ret = enable_phy(mdsi,
					 &shared_timings[DSI_CLOCK_MASTER]);
			if (ret)
				return ret;
			ret = enable_phy(sdsi,
					 &shared_timings[DSI_CLOCK_SLAVE]);
			if (ret) {
				msm_dsi_phy_disable(mdsi->phy);
				return ret;
			}
		}
	} else {
		msm_dsi_host_reset_phy(msm_dsi->host);
		ret = enable_phy(msm_dsi, &shared_timings[id]);
		if (ret)
			return ret;
	}

	msm_dsi->phy_enabled = true;

	return 0;
}

static void dsi_mgr_phy_disable(int id)
{
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *mdsi = dsi_mgr_get_dsi(DSI_CLOCK_MASTER);
	struct msm_dsi *sdsi = dsi_mgr_get_dsi(DSI_CLOCK_SLAVE);

	/* disable DSI phy
	 * In bonded dsi configuration, the phy should be disabled for the
	 * first controller only when the second controller is disabled.
	 */
	msm_dsi->phy_enabled = false;
	if (IS_BONDED_DSI() && mdsi && sdsi) {
		if (!mdsi->phy_enabled && !sdsi->phy_enabled) {
			msm_dsi_phy_disable(sdsi->phy);
			msm_dsi_phy_disable(mdsi->phy);
		}
	} else {
		msm_dsi_phy_disable(msm_dsi->phy);
	}
}

struct dsi_bridge {
	struct drm_bridge base;
	int id;
};

#define to_dsi_bridge(x) container_of(x, struct dsi_bridge, base)

static int dsi_mgr_bridge_get_id(struct drm_bridge *bridge)
{
	struct dsi_bridge *dsi_bridge = to_dsi_bridge(bridge);
	return dsi_bridge->id;
}

static int dsi_mgr_bridge_power_on(struct drm_bridge *bridge)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *msm_dsi1 = dsi_mgr_get_dsi(DSI_1);
	struct mipi_dsi_host *host = msm_dsi->host;
	struct msm_dsi_phy_shared_timings phy_shared_timings[DSI_MAX];
	bool is_bonded_dsi = IS_BONDED_DSI();
	int ret;

	DBG("id=%d", id);

	ret = dsi_mgr_phy_enable(id, phy_shared_timings);
	if (ret)
		goto phy_en_fail;

	ret = msm_dsi_host_power_on(host, &phy_shared_timings[id], is_bonded_dsi, msm_dsi->phy);
	if (ret) {
		pr_err("%s: power on host %d failed, %d\n", __func__, id, ret);
		goto host_on_fail;
	}

	if (is_bonded_dsi && msm_dsi1) {
		ret = msm_dsi_host_power_on(msm_dsi1->host,
				&phy_shared_timings[DSI_1], is_bonded_dsi, msm_dsi1->phy);
		if (ret) {
			pr_err("%s: power on host1 failed, %d\n",
							__func__, ret);
			goto host1_on_fail;
		}
	}

	/*
	 * Enable before preparing the panel, disable after unpreparing, so
	 * that the panel can communicate over the DSI link.
	 */
	msm_dsi_host_enable_irq(host);
	if (is_bonded_dsi && msm_dsi1)
		msm_dsi_host_enable_irq(msm_dsi1->host);

	return 0;

host1_on_fail:
	msm_dsi_host_power_off(host);
host_on_fail:
	dsi_mgr_phy_disable(id);
phy_en_fail:
	return ret;
}

static void dsi_mgr_bridge_power_off(struct drm_bridge *bridge)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *msm_dsi1 = dsi_mgr_get_dsi(DSI_1);
	struct mipi_dsi_host *host = msm_dsi->host;
	bool is_bonded_dsi = IS_BONDED_DSI();

	msm_dsi_host_disable_irq(host);
	if (is_bonded_dsi && msm_dsi1) {
		msm_dsi_host_disable_irq(msm_dsi1->host);
		msm_dsi_host_power_off(msm_dsi1->host);
	}
	msm_dsi_host_power_off(host);
	dsi_mgr_phy_disable(id);
}

/* blank-notify deferral work to prevent KMS atomic commit deadlock */
static struct work_struct dsi_blank_work;
static bool dsi_blank_work_inited;

static void dsi_blank_work_fn(struct work_struct *work)
{
	enum drm_notifier_data blank = MI_DRM_BLANK_UNBLANK;

	mi_drm_notifier_call_chain(MI_DRM_EVENT_BLANK, &blank);
}

static void dsi_mgr_bridge_enable(struct drm_bridge *bridge,
				  struct drm_atomic_commit *commit)
{
	if (!dsi_blank_work_inited) {
		INIT_WORK(&dsi_blank_work, dsi_blank_work_fn);
		dsi_blank_work_inited = true;
	}
	schedule_work(&dsi_blank_work);
}

static void dsi_mgr_bridge_pre_enable(struct drm_bridge *bridge,
				      struct drm_atomic_commit *commit)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *msm_dsi1 = dsi_mgr_get_dsi(DSI_1);
	struct mipi_dsi_host *host = msm_dsi->host;
	bool is_bonded_dsi = IS_BONDED_DSI();
	int ret;

	DBG("id=%d", id);

	/* Do nothing with the host if it is slave-DSI in case of bonded DSI */
	if (is_bonded_dsi && !IS_MASTER_DSI_LINK(id))
		return;

	ret = dsi_mgr_bridge_power_on(bridge);
	if (ret) {
		dev_err(&msm_dsi->pdev->dev, "Power on failed: %d\n", ret);
		return;
	}

	ret = msm_dsi_host_enable(host);
	if (ret) {
		pr_err("%s: enable host %d failed, %d\n", __func__, id, ret);
		goto host_en_fail;
	}

	if (is_bonded_dsi && msm_dsi1) {
		ret = msm_dsi_host_enable(msm_dsi1->host);
		if (ret) {
			pr_err("%s: enable host1 failed, %d\n", __func__, ret);
			goto host1_en_fail;
		}
	}

	return;

host1_en_fail:
	msm_dsi_host_disable(host);
host_en_fail:
	dsi_mgr_bridge_power_off(bridge);
}

void msm_dsi_manager_tpg_enable(void)
{
	struct msm_dsi *m_dsi = dsi_mgr_get_dsi(DSI_0);
	struct msm_dsi *s_dsi = dsi_mgr_get_dsi(DSI_1);

	/* if dual dsi, trigger tpg on master first then slave */
	if (m_dsi) {
		msm_dsi_host_test_pattern_en(m_dsi->host);
		if (IS_BONDED_DSI() && s_dsi)
			msm_dsi_host_test_pattern_en(s_dsi->host);
	}
}

static void dsi_mgr_bridge_post_disable(struct drm_bridge *bridge,
					struct drm_atomic_commit *commit)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *msm_dsi1 = dsi_mgr_get_dsi(DSI_1);
	struct mipi_dsi_host *host = msm_dsi->host;
	bool is_bonded_dsi = IS_BONDED_DSI();
	int ret;
	enum drm_notifier_data notifier_data;

	DBG("id=%d", id);

	notifier_data = MI_DRM_BLANK_POWERDOWN;
	mi_drm_notifier_call_chain(MI_DRM_EARLY_EVENT_BLANK, &notifier_data);

	/*
	 * Do nothing with the host if it is slave-DSI in case of bonded DSI.
	 * It is safe to call dsi_mgr_phy_disable() here because a single PHY
	 * won't be diabled until both PHYs request disable.
	 */
	if (is_bonded_dsi && !IS_MASTER_DSI_LINK(id))
		goto disable_phy;

	ret = msm_dsi_host_disable(host);
	if (ret)
		pr_err("%s: host %d disable failed, %d\n", __func__, id, ret);

	if (is_bonded_dsi && msm_dsi1) {
		ret = msm_dsi_host_disable(msm_dsi1->host);
		if (ret)
			pr_err("%s: host1 disable failed, %d\n", __func__, ret);
	}

	msm_dsi_host_disable_irq(host);
	if (is_bonded_dsi && msm_dsi1)
		msm_dsi_host_disable_irq(msm_dsi1->host);

	/* Save PHY status if it is a clock source */
	msm_dsi_phy_pll_save_state(msm_dsi->phy);

	ret = msm_dsi_host_power_off(host);
	if (ret)
		pr_err("%s: host %d power off failed,%d\n", __func__, id, ret);

	if (is_bonded_dsi && msm_dsi1) {
		ret = msm_dsi_host_power_off(msm_dsi1->host);
		if (ret)
			pr_err("%s: host1 power off failed, %d\n",
								__func__, ret);
	}

disable_phy:
	dsi_mgr_phy_disable(id);
}

static void dsi_mgr_bridge_mode_set(struct drm_bridge *bridge,
		const struct drm_display_mode *mode,
		const struct drm_display_mode *adjusted_mode)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *other_dsi = dsi_mgr_get_other_dsi(id);
	struct mipi_dsi_host *host = msm_dsi->host;
	bool is_bonded_dsi = IS_BONDED_DSI();

	DBG("set mode: " DRM_MODE_FMT, DRM_MODE_ARG(mode));

	if (is_bonded_dsi && !IS_MASTER_DSI_LINK(id))
		return;

	msm_dsi_host_set_display_mode(host, adjusted_mode);
	if (is_bonded_dsi && other_dsi)
		msm_dsi_host_set_display_mode(other_dsi->host, adjusted_mode);
}

static enum drm_mode_status dsi_mgr_bridge_mode_valid(struct drm_bridge *bridge,
						      const struct drm_display_info *info,
						      const struct drm_display_mode *mode)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct mipi_dsi_host *host = msm_dsi->host;
	struct platform_device *pdev = msm_dsi->pdev;
	struct dev_pm_opp *opp;
	unsigned long byte_clk_rate;

	byte_clk_rate = dsi_byte_clk_get_rate(host, IS_BONDED_DSI(), mode);

	opp = dev_pm_opp_find_freq_ceil(&pdev->dev, &byte_clk_rate);
	if (!IS_ERR(opp)) {
		dev_pm_opp_put(opp);
	} else if (PTR_ERR(opp) == -ERANGE) {
		/*
		 * An empty table is created by devm_pm_opp_set_clkname() even
		 * if there is none. Thus find_freq_ceil will still return
		 * -ERANGE in such case.
		 */
		if (dev_pm_opp_get_opp_count(&pdev->dev) != 0)
			return MODE_CLOCK_RANGE;
	} else {
			return MODE_ERROR;
	}

	return msm_dsi_host_check_dsc(host, mode);
}

static int dsi_mgr_bridge_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	int id = dsi_mgr_bridge_get_id(bridge);
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);

	return drm_bridge_attach(encoder, msm_dsi->next_bridge,
				 bridge, flags);
}

static const struct drm_bridge_funcs dsi_mgr_bridge_funcs = {
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.attach = dsi_mgr_bridge_attach,
	.atomic_pre_enable = dsi_mgr_bridge_pre_enable,
	.atomic_enable = dsi_mgr_bridge_enable,
	.atomic_post_disable = dsi_mgr_bridge_post_disable,
	.mode_set = dsi_mgr_bridge_mode_set,
	.mode_valid = dsi_mgr_bridge_mode_valid,
};

/* initialize bridge */
int msm_dsi_manager_connector_init(struct msm_dsi *msm_dsi,
				   struct drm_encoder *encoder)
{
	struct drm_device *dev = msm_dsi->dev;
	struct drm_bridge *bridge;
	struct dsi_bridge *dsi_bridge;
	struct drm_connector *connector;
	int ret;

	dsi_bridge = devm_drm_bridge_alloc(msm_dsi->dev->dev, struct dsi_bridge, base,
					   &dsi_mgr_bridge_funcs);
	if (IS_ERR(dsi_bridge))
		return PTR_ERR(dsi_bridge);

	dsi_bridge->id = msm_dsi->id;
	msm_dsi->encoder = encoder;

	bridge = &dsi_bridge->base;

	ret = devm_drm_bridge_add(msm_dsi->dev->dev, bridge);
	if (ret)
		return ret;

	ret = drm_bridge_attach(encoder, bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		return ret;

	connector = drm_bridge_connector_init(dev, encoder);
	if (IS_ERR(connector)) {
		DRM_ERROR("Unable to create bridge connector\n");
		return PTR_ERR(connector);
	}

	return 0;
}

int msm_dsi_manager_cmd_xfer(int id, const struct mipi_dsi_msg *msg)
{
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *msm_dsi1 = dsi_mgr_get_dsi(DSI_1);
	struct mipi_dsi_host *host = msm_dsi->host;
	bool is_read = (msg->rx_buf && msg->rx_len);
	bool need_sync = (IS_SYNC_NEEDED() && !is_read);
	int ret;

	if (!msg->tx_buf || !msg->tx_len)
		return 0;

	/* In bonded master case, panel requires the same commands sent to
	 * both DSI links. Host issues the command trigger to both links
	 * when DSI_0 calls the cmd transfer function, no matter it happens
	 * before or after DSI_1 cmd transfer.
	 */
	if (need_sync && (id == DSI_1))
		return is_read ? msg->rx_len : msg->tx_len;

	if (need_sync && msm_dsi1) {
		ret = msm_dsi_host_xfer_prepare(msm_dsi1->host, msg);
		if (ret) {
			pr_err("%s: failed to prepare non-trigger host, %d\n",
				__func__, ret);
			return ret;
		}
	}
	ret = msm_dsi_host_xfer_prepare(host, msg);
	if (ret) {
		pr_err("%s: failed to prepare host, %d\n", __func__, ret);
		goto restore_host0;
	}

	ret = is_read ? msm_dsi_host_cmd_rx(host, msg) :
			msm_dsi_host_cmd_tx(host, msg);

	msm_dsi_host_xfer_restore(host, msg);

restore_host0:
	if (need_sync && msm_dsi1)
		msm_dsi_host_xfer_restore(msm_dsi1->host, msg);

	return ret;
}

bool msm_dsi_manager_cmd_xfer_trigger(int id, u32 dma_base, u32 len)
{
	struct msm_dsi *msm_dsi = dsi_mgr_get_dsi(id);
	struct msm_dsi *msm_dsi1 = dsi_mgr_get_dsi(DSI_1);
	struct mipi_dsi_host *host = msm_dsi->host;

	if (IS_SYNC_NEEDED() && (id == DSI_1))
		return false;

	if (IS_SYNC_NEEDED() && msm_dsi1)
		msm_dsi_host_cmd_xfer_commit(msm_dsi1->host, dma_base, len);

	msm_dsi_host_cmd_xfer_commit(host, dma_base, len);

	return true;
}

static void dsi_mgr_dfps_sysfs_add(struct msm_dsi *msm_dsi);

int msm_dsi_manager_register(struct msm_dsi *msm_dsi)
{
	struct msm_dsi_manager *msm_dsim = &msm_dsim_glb;
	int id = msm_dsi->id;
	int ret;

	if (id >= DSI_MAX) {
		pr_err("%s: invalid id %d\n", __func__, id);
		return -EINVAL;
	}

	if (msm_dsim->dsi[id]) {
		pr_err("%s: dsi%d already registered\n", __func__, id);
		return -EBUSY;
	}

	msm_dsim->dsi[id] = msm_dsi;

	ret = dsi_mgr_parse_of(msm_dsi->pdev->dev.of_node, id);
	if (ret) {
		pr_err("%s: failed to parse OF DSI info\n", __func__);
		goto fail;
	}

	ret = dsi_mgr_setup_components(id);
	if (ret) {
		pr_err("%s: failed to register mipi dsi host for DSI %d: %d\n",
			__func__, id, ret);
		goto fail;
	}

	/* expose the runtime refresh-rate knob on the master link */
	if (id == msm_dsim->master_dsi_link_id)
		dsi_mgr_dfps_sysfs_add(msm_dsi);

	return 0;

fail:
	msm_dsim->dsi[id] = NULL;
	return ret;
}

void msm_dsi_manager_unregister(struct msm_dsi *msm_dsi)
{
	struct msm_dsi_manager *msm_dsim = &msm_dsim_glb;

	if (msm_dsi->host)
		msm_dsi_host_unregister(msm_dsi->host);

	if (msm_dsi->id >= 0)
		msm_dsim->dsi[msm_dsi->id] = NULL;
}

bool msm_dsi_is_bonded_dsi(struct msm_dsi *msm_dsi)
{
	return IS_BONDED_DSI();
}

bool msm_dsi_is_master_dsi(struct msm_dsi *msm_dsi)
{
	return IS_MASTER_DSI_LINK(msm_dsi->id);
}

const char *msm_dsi_get_te_source(struct msm_dsi *msm_dsi)
{
	return msm_dsi->te_source;
}

/**
 * msm_dsi_manager_dfps_set() - change the panel refresh rate at runtime
 * @new_fps: requested refresh rate in Hz
 *
 * Both links of a bonded DSI pair drive the same panel, so both controllers
 * have to be reprogrammed. The controller-side work lives in
 * msm_dsi_host_dfps_update().
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int msm_dsi_manager_dfps_apply(struct msm_dsi *master,
				      struct msm_dsi *slave, u32 fps, u32 cur_fps)
{
	int ret;

	/*
	 * Always move the DSI link first, then the DPU INTF.  Programming the
	 * interface before the controller leaves the pipeline unable to lock
	 * (measured: vsync collapses to ~3/s right after a 60 -> 120 switch,
	 * and the panel shows a duplicated image).  The earlier
	 * direction-dependent ordering did that on the shrink and is wrong.
	 */
	/*
	 * Begin both links (shadow timing armed), move the DPU interface, then
	 * commit both links.  This mirrors what the vendor gets for free by
	 * doing the whole switch inside one commit: DSI and INTF always change
	 * on the same frame.
	 */
	ret = msm_dsi_host_dfps_update(master->host, fps, IS_BONDED_DSI(), false);
	if (ret)
		return ret;

	if (IS_BONDED_DSI() && slave && slave->host) {
		ret = msm_dsi_host_dfps_update(slave->host, fps, true, false);
		if (ret)
			return ret;
	}

	if (msm_dfps_dpu_hook && master->encoder)
		msm_dfps_dpu_hook(master->encoder, fps);

	/* latch is deferred to msm_dsi_manager_dfps_latch() after kickoff */
	return 0;
}

/*
 * Latch the DSI timing programmed by msm_dsi_manager_dfps_commit_apply().
 * Kept separate so it can run *after* commit_modeset_enables() (i.e. after
 * the DPU kickoff): that is the vendor's post-kickoff ordering, and it is
 * what stops the interface and the controller from disagreeing for one frame.
 */
/*
 * Send the touch sync-pen between the kickoff and the DSI timing latch.
 * Vendor does exactly this (sde_encoder_kickoff: kickoff -> pen -> later
 * DB=0), so the DDIC learns about the new rate while the old timing is
 * still in effect; doing it after DB=0 lets the panel process one frame
 * with the new timings but the old internal state - the 60 -> 120 repeat.
 */
void msm_dsi_manager_dfps_pen(struct drm_device *dev, u32 fps)
{
	struct msm_dsi *master;

	master = dsi_mgr_get_dsi(msm_dsim_glb.master_dsi_link_id);
	if (!master || !master->host || !fps)
		return;

	msm_dsi_host_wait_vfp(master->host);

	if (msm_dfps_sync_pen_hook)
		msm_dfps_sync_pen_hook(fps);
}

void msm_dsi_manager_dfps_latch(struct drm_device *dev, u32 fps)
{
	struct msm_dsi *master;
	struct msm_dsi *slave;

	master = dsi_mgr_get_dsi(msm_dsim_glb.master_dsi_link_id);
	if (!master || !master->host)
		return;

	slave = IS_BONDED_DSI() ?
		dsi_mgr_get_other_dsi(msm_dsim_glb.master_dsi_link_id) : NULL;

	msm_dsi_host_dfps_latch(master->host);
	if (slave && slave->host)
		msm_dsi_host_dfps_latch(slave->host);

	(void)fps;
}

static int __maybe_unused msm_dsi_manager_dfps_set(u32 new_fps)
{
	struct msm_dsi *master;
	struct msm_dsi *slave;
	u32 cur_fps;

	master = dsi_mgr_get_dsi(msm_dsim_glb.master_dsi_link_id);
	if (!master || !master->host)
		return -ENODEV;

	slave = IS_BONDED_DSI() ?
		dsi_mgr_get_other_dsi(msm_dsim_glb.master_dsi_link_id) : NULL;

	cur_fps = msm_dsi_host_dfps_get_fps(master->host);
	if (!cur_fps || cur_fps == new_fps)
		return 0;

	/*
	 * One shot: the panel is a TDDI part and every intermediate timing is
	 * a window in which the touch controller sees a frame it cannot
	 * sync to, so stepping the change makes things worse, not better.
	 */
	return msm_dsi_manager_dfps_apply(master, slave, new_fps, cur_fps);
}

/*
 * msm_dsi_manager_dfps_commit_apply() - apply a pending dfps change
 * @dev: DRM device
 * @fps: requested refresh rate
 *
 * Called from msm_atomic_commit_tail() after the planes have been programmed
 * and before the flush goes out, so the DSI timing and the DPU interface
 * timing are latched by the same commit instead of one frame apart. That
 * one-frame skew is what the panel shows as a flash when going 60 -> 120.
 */
u32 msm_dsi_manager_dfps_commit_apply(struct drm_device *dev, u32 fps)
{
	struct msm_dsi *master;
	struct msm_dsi *slave;
	u32 cur_fps;

	master = dsi_mgr_get_dsi(msm_dsim_glb.master_dsi_link_id);
	if (!master || !master->host)
		return 0;

	slave = IS_BONDED_DSI() ?
		dsi_mgr_get_other_dsi(msm_dsim_glb.master_dsi_link_id) : NULL;

	cur_fps = msm_dsi_host_dfps_get_fps(master->host);
	if (!cur_fps)
		return 0;

	if (cur_fps == fps) {
		/*
		 * Same rate: no timing change, but re-apply the touch sync
		 * page anyway.  The DDIC forgets page 0x2A / reg 0x23 when it
		 * is powered down on blank; echoing the current rate is the
		 * cheapest way to put it back without a full dfps switch.
		 */
		msm_dsi_manager_dfps_pen(dev, fps);
		return 0;
	}

	msm_dsi_manager_dfps_apply(master, slave, fps, cur_fps);

	return fps;
}

static ssize_t dfps_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct msm_dsi *msm_dsi = dev_get_drvdata(dev);
	struct drm_device *drm_dev;
	struct drm_atomic_commit *state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc *crtc;
	struct msm_drm_private *priv;
	u32 fps, old_fps;
	int ret;

	ret = kstrtou32(buf, 10, &fps);
	if (ret)
		return ret;

	if (!msm_dsi || !msm_dsi->dev)
		return -ENODEV;

	drm_dev = msm_dsi->dev;
	priv = drm_dev->dev_private;
	if (!priv || !priv->kms)
		return -ENODEV;

	old_fps = msm_dsi_host_dfps_get_fps(msm_dsi->host);

	priv->kms->pending_dfps_fps = fps;

	dev_info(dev, "dfps_store: id=%d requested %u Hz (from %u)\n",
		 msm_dsi->id, fps, old_fps);

	/*
	 * Nothing else in the system changes, so build a commit of our own and
	 * let msm_atomic_commit_tail() apply the pending rate while it holds
	 * the CRTC locks and is about to flush.
	 */
	state = drm_atomic_commit_alloc(drm_dev);
	if (!state)
		return -ENOMEM;

	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
retry:
	ret = drm_modeset_lock_all_ctx(drm_dev, &ctx);
	if (!ret) {
		struct drm_crtc_state *crtc_state;

		drm_for_each_crtc(crtc, drm_dev) {
			crtc_state = drm_atomic_get_crtc_state(state, crtc);
			if (IS_ERR(crtc_state)) {
				ret = PTR_ERR(crtc_state);
				break;
			}
		}
	}
	if (!ret)
		ret = drm_atomic_commit(state);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	drm_atomic_commit_put(state);

	return ret ? ret : len;
}
static DEVICE_ATTR_WO(dfps);

static void dsi_mgr_dfps_sysfs_add(struct msm_dsi *msm_dsi)
{
	if (device_create_file(&msm_dsi->pdev->dev, &dev_attr_dfps))
		pr_warn("dsi%d: failed to create dfps sysfs entry\n",
			msm_dsi->id);
}
