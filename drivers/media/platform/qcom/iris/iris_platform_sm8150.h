/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __IRIS_PLATFORM_SM8150_H__
#define __IRIS_PLATFORM_SM8150_H__

static const char * const sm8150_clk_reset_table[] = {
	"bus", "core", "vcodec0_bus", "vcodec1_bus"
};

static const char * const sm8150_pmdomain_table[] = {
	"venus", "vcodec0", "vcodec1"
};

static const char * const sm8150_opp_pd_table[] = { "cx" };

static const struct platform_clk_data sm8150_clk_table[] = {
	{IRIS_AXI_CLK,    "iface"        },
	{IRIS_AXI1_CLK,   "bus"          },
	{IRIS_CTRL_CLK,   "core"         },
	{IRIS_HW_AHB_CLK, "vcodec0_bus"  },
	{IRIS_HW_CLK,     "vcodec0_core" },
	{IRIS_HW_AHB_CLK, "vcodec1_bus"  },
	{IRIS_HW_CLK,     "vcodec1_core" },
};

static const char * const sm8150_opp_clk_table[] = {
	"core",
	NULL,
};

#endif
