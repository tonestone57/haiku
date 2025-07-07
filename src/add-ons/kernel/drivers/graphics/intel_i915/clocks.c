/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "clocks.h"
#include "intel_i915_priv.h"
#include "registers.h"
#include "forcewake.h"

#include <KernelExport.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>  // For snprintf in TRACE (already included by priv.h)
#include <math.h>   // For round, if used (though integer math preferred)
#include <kernel/util/gcd.h> // For gcd


// Reference clocks (kHz)
#define REF_CLOCK_SSC_96000_KHZ   96000
#define REF_CLOCK_SSC_120000_KHZ 120000
#define REF_CLOCK_LCPLL_1350_MHZ_KHZ 1350000
#define REF_CLOCK_LCPLL_2700_MHZ_KHZ 2700000

// WRPLL VCO constraints for Gen7 (kHz)
#define WRPLL_VCO_MIN_KHZ   2700000
#define WRPLL_VCO_MAX_KHZ   5400000 // Can be higher on some HSW SKUs (e.g. 6.48 GHz)

// SPLL VCO constraints for HSW (kHz)
#define SPLL_VCO_MIN_KHZ_HSW    2700000
#define SPLL_VCO_MAX_KHZ_HSW    5400000


// --- Helper: Read current CDCLK ---
static uint32_t read_current_cdclk_khz(intel_i915_device_info* devInfo) {
	// Callers of this helper should handle forcewake
	if (IS_HASWELL(devInfo->device_id)) {
		uint32_t lcpll1_ctl = intel_i915_read32(devInfo, LCPLL_CTL); // LCPLL1_CTL for HSW
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_HSW);
		uint32_t lcpll_source_for_cdclk_khz;

		uint32_t cdclk_lcpll_select = cdclk_ctl & HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT;
		if (cdclk_lcpll_select == HSW_CDCLK_SELECT_2700) lcpll_source_for_cdclk_khz = 2700000;
		else if (cdclk_lcpll_select == HSW_CDCLK_SELECT_810) lcpll_source_for_cdclk_khz = 810000;
		else lcpll_source_for_cdclk_khz = 1350000;
		TRACE("Clocks: HSW CDCLK LCPLL source selected by CDCLK_CTL[26]=%u is %u kHz. (LCPLL1_CTL=0x%x for WRPLL ref)\n",
			cdclk_lcpll_select >> 26, lcpll_source_for_cdclk_khz, lcpll1_ctl);

		uint32_t cdclk_divisor_sel = cdclk_ctl & HSW_CDCLK_FREQ_SEL_MASK;
		switch (cdclk_divisor_sel) {
			case HSW_CDCLK_DIVISOR_3_FIELD_VAL:   return lcpll_source_for_cdclk_khz / 3;
			case HSW_CDCLK_DIVISOR_2_5_FIELD_VAL: return (uint32_t)(lcpll_source_for_cdclk_khz / 2.5);
			case HSW_CDCLK_DIVISOR_4_FIELD_VAL:   return lcpll_source_for_cdclk_khz / 4;
			case HSW_CDCLK_DIVISOR_2_FIELD_VAL:   return lcpll_source_for_cdclk_khz / 2;
		}
		TRACE("Clocks: HSW CDCLK_CTL unknown divisor sel 0x%x, defaulting CDCLK to LCPLL_src/3\n", cdclk_divisor_sel);
		return lcpll_source_for_cdclk_khz / 3;

	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
		if (IS_IVYBRIDGE_MOBILE(devInfo->device_id)) {
			switch (cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK_MOBILE) {
				case CDCLK_FREQ_337_5_MHZ_IVB_M: return 337500;
				case CDCLK_FREQ_450_MHZ_IVB_M:   return 450000;
				case CDCLK_FREQ_540_MHZ_IVB_M:   return 540000;
				case CDCLK_FREQ_675_MHZ_IVB_M:   return 675000;
				default: TRACE("Clocks: IVB Mobile CDCLK_CTL unknown value 0x%x\n", cdclk_ctl); return 450000;
			}
		} else { // IVB Desktop/Server
			uint32_t sel_desktop = cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK_DESKTOP;
			if (sel_desktop == CDCLK_FREQ_400_IVB_D) return 400000;
			if (sel_desktop == CDCLK_FREQ_320_IVB_D) return 320000;
			TRACE("Clocks: IVB Desktop CDCLK_CTL unknown value 0x%x\n", cdclk_ctl); return 400000;
		}
	}
	TRACE("Clocks: Unknown GEN for CDCLK read, defaulting.\n");
	return 450000;
}

status_t intel_i915_clocks_init(intel_i915_device_info* devInfo) {
	if (!devInfo || !devInfo->mmio_regs_addr) return B_NO_INIT;
	status_t status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) { TRACE("Clocks_init: Failed to get forcewake: %s\n", strerror(status)); return status;}
	devInfo->current_cdclk_freq_khz = read_current_cdclk_khz(devInfo);
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("Clocks: Current CDCLK read as %" B_PRIu32 " kHz.\n", devInfo->current_cdclk_freq_khz);
	return B_OK;
}
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo) { /* ... */ }

static uint32_t get_hsw_lcpll_link_rate_khz(intel_i915_device_info* devInfo) {
	if (!IS_HASWELL(devInfo->device_id)) { TRACE("Clocks: get_hsw_lcpll_link_rate_khz on non-HSW\n"); return 0;}
	uint32_t lcpll_ctl = intel_i915_read32(devInfo, LCPLL_CTL);
	if (!(lcpll_ctl & LCPLL_PLL_ENABLE) || !(lcpll_ctl & LCPLL_PLL_LOCK)) {
		TRACE("Clocks: HSW LCPLL1 not enabled/locked (0x%08x)\n", lcpll_ctl); return 2700000;
	}
	uint32_t link_rate_bits = lcpll_ctl & LCPLL1_LINK_RATE_HSW_MASK; uint32_t freq_khz = 0;
	switch (link_rate_bits) {
		case LCPLL_LINK_RATE_810:  freq_khz = 810000; break; case LCPLL_LINK_RATE_1350: freq_khz = 1350000; break;
		case LCPLL_LINK_RATE_1620: freq_khz = 1620000; break; case LCPLL_LINK_RATE_2700: freq_khz = 2700000; break;
		default: TRACE("Clocks: HSW LCPLL1_CTL (0x%08x) unknown rate bits 0x%x\n", lcpll_ctl, link_rate_bits); freq_khz = 2700000;
	}
	TRACE("Clocks: HSW LCPLL1 rate: %u kHz (LCPLL_CTL=0x%08x)\n", freq_khz, lcpll_ctl); return freq_khz;
}

static const int gen7_wrpll_p1_map[] = {1, 2, 3, 4};
static const int gen7_wrpll_p2_eff_div[] = {5, 10};
static bool find_gen7_wrpll_dividers(uint32_t tclk, uint32_t rclk, intel_clock_params_t* p, bool isdp) {
	long best_err = 1000000; p->is_wrpll = true; uint32_t v_min=WRPLL_VCO_MIN_KHZ, v_max=WRPLL_VCO_MAX_KHZ;
	for (int p1i=0;p1i<4;++p1i) for (int p2i=0;p2i<2;++p2i) {
		uint32_t p1=gen7_wrpll_p1_map[p1i], p2d=gen7_wrpll_p2_eff_div[p2i], pt=p1*p2d, tvco=tclk*pt;
		if(tvco<v_min||tvco>v_max)continue;
		for(uint32_t n=1;n<=15;++n){
			double meff=(double)tvco*n/(2.0*rclk); uint32_t m2i=(uint32_t)floor(meff);
			uint32_t m2f=(uint32_t)round((meff-m2i)*1024.0); if(m2f>1023)m2f=1023;
			if(m2i<16||m2i>127)continue;
			uint32_t avco; bool uf=(m2f>0&&m2f<1024);
			if(uf)avco=(uint32_t)round((double)rclk*2.0*(m2i+(double)m2f/1024.0)/n); else avco=(rclk*2*m2i)/n;
			long err=abs((long)avco-(long)tvco);
			if(err<best_err){best_err=err;p->dpll_vco_khz=avco;p->wrpll_n=n;p->wrpll_m2=m2i;
			p->wrpll_m2_frac_en=uf;p->wrpll_m2_frac=uf?m2f:0;p->wrpll_p1=p1i;p->wrpll_p2=p2i;}
			if(best_err==0&&!isdp)goto found_gen7w;
		} if(best_err==0&&!isdp)goto found_gen7w;
	} found_gen7w:; long allow_err=min_c(tclk/1000,500);
	if(p->dpll_vco_khz>0&&best_err<=allow_err){ TRACE("WRPLL: t%u r%u->V%u N%u M2i%u Fe%d F%u P1f%u P2f%u (e%ld)\n",tclk,rclk,p->dpll_vco_khz,p->wrpll_n,p->wrpll_m2,p->wrpll_m2_frac_en,p->wrpll_m2_frac,p->wrpll_p1,p->wrpll_p2,best_err); return true;}
	TRACE("WRPLL FAIL: t%u r%u. BestE %ld (allow %ld)\n",tclk,rclk,best_err,allow_err); return false;
}
static const int hsw_spll_p1_map[] = {1,2,3,5}; static const int hsw_spll_p2_eff_div[] = {5,10};
static bool find_hsw_spll_dividers(uint32_t tclk,uint32_t rclk,intel_clock_params_t*p){
	long best_err=1000000;p->is_wrpll=false;uint32_t v_min=SPLL_VCO_MIN_KHZ_HSW,v_max=SPLL_VCO_MAX_KHZ_HSW;
	for(int p1i=0;p1i<4;++p1i)for(int p2i=0;p2i<2;++p2i){
		uint32_t p1a=hsw_spll_p1_map[p1i],p2d=hsw_spll_p2_eff_div[p2i],pt=p1a*p2d,tvco=tclk*pt;
		if(tvco<v_min||tvco>v_max)continue;
		for(uint32_t na=1;na<=15;++na){ if(na<1)continue;
			uint64_t m2n=(uint64_t)tvco*na;uint32_t m2d=rclk*2,m2i=(m2n+m2d/2)/m2d;
			if(m2i<20||m2i>120)continue;
			uint32_t avco=(rclk*2*m2i)/na;long err=abs((long)avco-(long)tvco);
			if(err<best_err){best_err=err;p->dpll_vco_khz=avco;p->spll_n=na-1;
			p->spll_m2=m2i;p->spll_p1=p1i;p->spll_p2=p2i;}
			if(best_err==0)goto found_hsw_spll;
		}if(best_err==0)goto found_hsw_spll;
	}found_hsw_spll:;
	if(p->dpll_vco_khz>0&&best_err<(tclk/1000)){TRACE("HSW SPLL: t%u r%u->V%u Nf%u M2i%u P1f%u P2f%u (e%ld)\n",tclk,rclk,p->dpll_vco_khz,p->spll_n,p->spll_m2,p->spll_p1,p->spll_p2,best_err);return true;}
	TRACE("HSW SPLL FAIL: t%u r%u. BestE %ld\n",tclk,rclk,best_err);return false;
}
status_t find_ivb_dpll_dividers(uint32_t t_out_clk, uint32_t rclk, bool isdp, intel_clock_params_t*p){
	const uint32_t v_min=1700000,v_max=3500000;long best_err=-1;p->is_wrpll=true;p->dpll_vco_khz=0;p->ivb_dpll_m1_reg_val=10;
	TRACE("IVB DPLL: Target %u Ref %u DP %d\n",t_out_clk,rclk,isdp);
	for(uint32_t p1f=0;p1f<=7;++p1f){uint32_t p1a=p1f+1,p2fv,p2a,p2ls=0,p2le=1;
		if(isdp){p2fv=1;p2a=5;p2ls=p2fv;p2le=p2fv;}
		for(uint32_t p2fi=p2ls;p2fi<=p2le;++p2fi){if(!isdp){p2fv=p2fi;p2a=(p2fv==1)?5:10;}
			uint64_t tv64=(uint64_t)t_out_clk*p1a*p2a;if(tv64<v_min||tv64>v_max)continue; uint32_t tv=(uint32_t)tv64;
			for(uint32_t n1f=0;n1f<=15;++n1f){uint32_t n1a=n1f+2; double m2id_f=(double)tv*n1a/rclk; int32_t m2fc=(int32_t)round(m2id_f)-2;
				for(int m2off=-2;m2off<=2;++m2off){int32_t m2fs=m2fc+m2off;if(m2fs<0||m2fs>511)continue;
					uint32_t m2f=(uint32_t)m2fs,m2a=m2f+2;uint64_t cvn=(uint64_t)rclk*m2a;uint32_t cv=(uint32_t)((cvn+(n1a/2))/n1a);
					uint64_t con=(uint64_t)cv;uint32_t pta=p1a*p2a;if(pta==0)continue;uint32_t coc=(uint32_t)((con+(pta/2))/pta);
					long err=(long)coc-(long)t_out_clk;if(err<0)err=-err;
					if(best_err==-1||err<best_err){best_err=err;p->dpll_vco_khz=cv;p->wrpll_p1=p1f;p->wrpll_p2=p2fv;p->wrpll_n=n1f;p->wrpll_m2=m2f;}
					if(err==0)goto found_ivb_final;
	}}}} found_ivb_final:;
	if(best_err==-1||best_err>1){TRACE("IVB DPLL FAIL: BestE %ld Tgt %u\n",best_err,t_out_clk);return B_ERROR;}
	TRACE("IVB DPLL: Tgt %u->VCO %u (Err %ld) P1f%u P2f%u N1f%u M2f%u M1f%u\n",t_out_clk,p->dpll_vco_khz,best_err,p->wrpll_p1,p->wrpll_p2,p->wrpll_n,p->wrpll_m2,p->ivb_dpll_m1_reg_val);return B_OK;
}

// --- FDI M/N Calculation Helpers ---
#define FDI_LINK_M_N_MASK ((1 << 16) -1)
static uint32_t fdi_m_n_calc_m_input_debug = 0;

static void intel_reduce_fdi_m_n_ratio(uint32_t *num, uint32_t *den) {
	while (*num > FDI_LINK_M_N_MASK || *den > FDI_LINK_M_N_MASK) {
		uint64_t common_divisor = gcd(*num, *den);
		if (common_divisor > 1) { *num /= common_divisor; *den /= common_divisor; }
		if (*num > FDI_LINK_M_N_MASK || *den > FDI_LINK_M_N_MASK) {
			if (*num > FDI_LINK_M_N_MASK && (*den <= FDI_LINK_M_N_MASK || *num > *den)) { *num >>= 1; if (*den > 1 && (*num %*den !=0)) *den >>=1; }
			else if (*den > FDI_LINK_M_N_MASK) {*den >>=1; if (*num > 1 && (*den %*num !=0)) *num >>=1;}
			else { *num >>= 1; *den >>= 1;}
		}
		if (*den == 0) { *den = 1; TRACE("FDI M/N: Denom became 0!\n");}
		if (*num == 0 && (fdi_m_n_calc_m_input_debug > 0)) {*num = 1; TRACE("FDI M/N: Num became 0 for non-zero input!\n");}
	}
}
static void calculate_fdi_m_n_values_internal(uint32_t *rM, uint32_t *rN, uint32_t mIn, uint32_t nIn) {
	fdi_m_n_calc_m_input_debug = mIn; if (nIn==0) {TRACE("FDI M/N: nIn is 0!\n");*rM=1;*rN=1;return;}
	*rM=mIn; *rN=nIn; intel_reduce_fdi_m_n_ratio(rM,rN);
	if(mIn>0&&*rM==0)*rM=1; if(nIn>0&&*rN==0)*rN=1;
	TRACE("FDI M/N Calc: In %lu/%lu -> Out %lu/%lu\n", mIn,nIn,*rM,*rN);
}
static void calculate_fdi_m_n_params(intel_i915_device_info* d, enum pipe_id_priv pi, intel_clock_params_t* c) {
	if(!c->needs_fdi){c->fdi_params.data_m=c->fdi_params.data_n=0;c->fdi_params.link_m=c->fdi_params.link_n=0;c->fdi_params.tu_size=64;return;}
	uint32_t pclk_khz=c->adjusted_pixel_clock_khz; uint32_t tconf=intel_i915_read32(d,TRANSCONF(pi));
	uint32_t bpc_f=(tconf&TRANSCONF_PIPE_BPC_MASK)>>TRANSCONF_PIPE_BPC_SHIFT;
	switch(bpc_f){case TRANSCONF_PIPE_BPC_6_FIELD:c->fdi_params.pipe_bpc_total=18;break; case TRANSCONF_PIPE_BPC_8_FIELD:c->fdi_params.pipe_bpc_total=24;break;
	case TRANSCONF_PIPE_BPC_10_FIELD:c->fdi_params.pipe_bpc_total=30;break; case TRANSCONF_PIPE_BPC_12_FIELD:c->fdi_params.pipe_bpc_total=36;break;
	default:c->fdi_params.pipe_bpc_total=24;TRACE("FDI M/N: Unknown BPC field %u, default 24bpp\n",bpc_f);}
	uint8_t lanes=c->fdi_params.fdi_lanes; if(lanes==0)lanes=(IS_IVYBRIDGE(d->device_id)&&!IS_IVYBRIDGE_MOBILE(d->device_id))?4:2;
	c->fdi_params.fdi_lanes=lanes; c->fdi_params.tu_size=64; uint32_t fdi_sym_rate=FDI_LINK_FREQ_2_7_GHZ_KHZ;
	if(pclk_khz==0||c->fdi_params.pipe_bpc_total==0){TRACE("FDI M/N: Invalid pclk(%u)/bpc(%u)\n",pclk_khz,c->fdi_params.pipe_bpc_total);c->fdi_params.data_m=22;c->fdi_params.data_n=24;c->fdi_params.link_m=22;c->fdi_params.link_n=24;return;}
	uint64_t n_num=(uint64_t)fdi_sym_rate*8*c->fdi_params.tu_size*lanes; uint64_t n_den=(uint64_t)pclk_khz*c->fdi_params.pipe_bpc_total;
	if(n_den==0){TRACE("FDI M/N: Denom 0 in N calc\n");c->fdi_params.data_n=1;}else{c->fdi_params.data_n=(uint16_t)(n_num/n_den);}
	c->fdi_params.data_m=c->fdi_params.tu_size;
	if(c->fdi_params.data_n==0)c->fdi_params.data_n=1; if(c->fdi_params.data_m==0)c->fdi_params.data_m=1;
	if(c->fdi_params.data_n>FDI_LINK_M_N_MASK)c->fdi_params.data_n=FDI_LINK_M_N_MASK; if(c->fdi_params.data_m>FDI_LINK_M_N_MASK)c->fdi_params.data_m=FDI_LINK_M_N_MASK;
	c->fdi_params.link_m=c->fdi_params.data_m; c->fdi_params.link_n=c->fdi_params.data_n;
	TRACE("FDI M/N: DataM/N=%u/%u,TU %u,BPC %u,Lanes %u,Pclk %u\n",c->fdi_params.data_m,c->fdi_params.data_n,c->fdi_params.tu_size,c->fdi_params.pipe_bpc_total,lanes,pclk_khz);
}
// --- END FDI M/N Calculation ---

status_t
intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv pipe,
	enum intel_port_id_priv targetPortId, intel_clock_params_t* clocks)
{
	memset(clocks, 0, sizeof(intel_clock_params_t));
	clocks->pixel_clock_khz = mode->timing.pixel_clock;
	clocks->adjusted_pixel_clock_khz = mode->timing.pixel_clock;
	clocks->needs_fdi = false;
	clocks->pipe_for_fdi_mn_calc = pipe;

	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);
	if (port_state == NULL) { TRACE("calculate_clocks: No port_state for targetPortId %d\n", targetPortId); return B_BAD_VALUE;}

	if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) {
		if (port_state->is_pch_port) {
			clocks->needs_fdi = true;
			clocks->fdi_params.fdi_lanes = (IS_IVYBRIDGE(devInfo->device_id) && !IS_IVYBRIDGE_MOBILE(devInfo->device_id)) ? 4 : 2;
			TRACE("calculate_clocks: Port ID %d PCH-driven, FDI needed (%d lanes)\n", targetPortId, clocks->fdi_params.fdi_lanes);
		}
	}
	clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
	if (clocks->cdclk_freq_khz == 0) {
		clocks->cdclk_freq_khz = IS_HASWELL(devInfo->device_id) ? 450000 : 400000;
		TRACE("calculate_clocks: CDCLK was 0, fallback %u kHz for Gen %d\n", clocks->cdclk_freq_khz, INTEL_DISPLAY_GEN(devInfo));
	}
	if (IS_HASWELL(devInfo->device_id)) {
		bool found_hsw_cdclk_setting = false; uint32_t target_cdclk = clocks->cdclk_freq_khz;
		uint32_t lcpll_sources[]={1350000,2700000,810000}; uint32_t select_bits[]={HSW_CDCLK_SELECT_1350,HSW_CDCLK_SELECT_2700,HSW_CDCLK_SELECT_810};
		for(int i=0;i<3;++i){ uint32_t src_khz=lcpll_sources[i], sel_val=select_bits[i], div_fval=0;
			if(target_cdclk==src_khz/2)div_fval=HSW_CDCLK_DIVISOR_2_FIELD_VAL; else if(target_cdclk==(uint32_t)(src_khz/2.5))div_fval=HSW_CDCLK_DIVISOR_2_5_FIELD_VAL;
			else if(target_cdclk==src_khz/3)div_fval=HSW_CDCLK_DIVISOR_3_FIELD_VAL; else if(target_cdclk==src_khz/4)div_fval=HSW_CDCLK_DIVISOR_4_FIELD_VAL;
			if(div_fval!=0||(target_cdclk==src_khz/3&&HSW_CDCLK_DIVISOR_3_FIELD_VAL==0)){
				if(target_cdclk==src_khz/3&&HSW_CDCLK_DIVISOR_3_FIELD_VAL==0)div_fval=HSW_CDCLK_DIVISOR_3_FIELD_VAL;
				clocks->hsw_cdclk_source_lcpll_freq_khz=src_khz; clocks->hsw_cdclk_ctl_field_val=sel_val|div_fval;
				clocks->hsw_cdclk_ctl_field_val&=~HSW_CDCLK_FREQ_DECIMAL_ENABLE; found_hsw_cdclk_setting=true;
				TRACE("calc_clk: HSW CDCLK: Target %u from LCPLL %u. CTL val: 0x%x\n",target_cdclk,src_khz,clocks->hsw_cdclk_ctl_field_val); break;
		}}
		if(!found_hsw_cdclk_setting){TRACE("calc_clk: HSW No LCPLL/div for CDCLK %u. Using current HW val.\n",target_cdclk);
			clocks->hsw_cdclk_ctl_field_val=intel_i915_read32(devInfo,CDCLK_CTL_HSW);
			uint32_t cur_sel=clocks->hsw_cdclk_ctl_field_val&HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT;
			if(cur_sel==HSW_CDCLK_SELECT_2700)clocks->hsw_cdclk_source_lcpll_freq_khz=2700000;
			else if(cur_sel==HSW_CDCLK_SELECT_810)clocks->hsw_cdclk_source_lcpll_freq_khz=810000;
			else clocks->hsw_cdclk_source_lcpll_freq_khz=1350000;
	}}

	bool is_dp= (port_state->type==PRIV_OUTPUT_DP||port_state->type==PRIV_OUTPUT_EDP);
	clocks->is_dp_or_edp=is_dp; clocks->is_lvds=(port_state->type==PRIV_OUTPUT_LVDS);
	uint32_t ref_clk=0, dpll_tgt_freq=clocks->adjusted_pixel_clock_khz;

	if(IS_HASWELL(devInfo->device_id)){ bool use_spll=false;
		// On Haswell, HDMI on DDI A (hw_port_index 0) typically uses SPLL.
		// Other DDIs (B, C, D, E) for HDMI/DP would use WRPLLs.
		// This is a common hardware configuration. VBT indicates which DDI is HDMI,
		// but usually not a specific flag to choose SPLL vs WRPLL for DDI A.
		// The VBT's main role here is to confirm that DDI A is indeed an HDMI port.
		if(port_state->type==PRIV_OUTPUT_HDMI && port_state->hw_port_index==0) { // DDI A as HDMI
			use_spll=true;
		}
		if(use_spll){clocks->selected_dpll_id=DPLL_ID_SPLL_HSW;clocks->is_wrpll=false;
			uint32_t spll_ctl=intel_i915_read32(devInfo,SPLL_CTL_HSW);
			if((spll_ctl&SPLL_REF_SEL_MASK_HSW)==SPLL_REF_LCPLL_HSW){ref_clk=get_hsw_lcpll_link_rate_khz(devInfo); TRACE("Clocks: HSW SPLL using LCPLL ref: %u kHz\n",ref_clk);}
			else{ref_clk=REF_CLOCK_SSC_96000_KHZ; TRACE("Clocks: HSW SPLL using SSC ref: %u kHz (Default)\n",ref_clk);}
			if(ref_clk==0){ref_clk=REF_CLOCK_SSC_96000_KHZ;TRACE("Clocks: HSW SPLL ref 0, fallback 96MHz SSC\n");}
			TRACE("Clocks: HSW HDMI Port A: SPLL, Target %u, Ref %u\n",dpll_tgt_freq,ref_clk);
			if(!find_hsw_spll_dividers(dpll_tgt_freq,ref_clk,clocks)){TRACE("Clocks: HSW SPLL calc fail\n");return B_ERROR;}
		}else{clocks->is_wrpll=true;
			if(pipe==PRIV_PIPE_A||pipe==PRIV_PIPE_C)clocks->selected_dpll_id=DPLL_ID_WRPLL1_HSW; else if(pipe==PRIV_PIPE_B)clocks->selected_dpll_id=DPLL_ID_WRPLL2_HSW; else return B_BAD_VALUE;
			clocks->lcpll_freq_khz=get_hsw_lcpll_link_rate_khz(devInfo); ref_clk=clocks->lcpll_freq_khz;
			if(is_dp){ if(port_state->dp_max_link_rate>=DPCD_LINK_BW_5_4)clocks->dp_link_rate_khz=540000; else if(port_state->dp_max_link_rate>=DPCD_LINK_BW_2_7)clocks->dp_link_rate_khz=270000; else clocks->dp_link_rate_khz=162000; dpll_tgt_freq=5400000;}
			TRACE("Clocks: HSW WRPLL%d, Ref(LCPLL) %u, DPLL Target %u\n",clocks->selected_dpll_id+1,ref_clk,dpll_tgt_freq);
			if(!find_gen7_wrpll_dividers(dpll_tgt_freq,ref_clk,clocks,is_dp))return B_ERROR;
	}}else if(IS_IVYBRIDGE(devInfo->device_id)){clocks->is_wrpll=true;
		if(pipe==PRIV_PIPE_A||pipe==PRIV_PIPE_C)clocks->selected_dpll_id=DPLL_ID_DPLL_A_IVB; else if(pipe==PRIV_PIPE_B)clocks->selected_dpll_id=DPLL_ID_DPLL_B_IVB; else return B_BAD_VALUE;
		bool use_ssc=true; ref_clk=use_ssc?REF_CLOCK_SSC_96000_KHZ:120000; clocks->ivb_dpll_m1_reg_val=use_ssc?10:8;
		if(is_dp){if(port_state->dp_max_link_rate>=DPCD_LINK_BW_2_7)clocks->dp_link_rate_khz=270000; else clocks->dp_link_rate_khz=162000; dpll_tgt_freq=clocks->dp_link_rate_khz;}
		TRACE("Clocks: IVB DPLL%c, Ref %u (SSC %d), DPLL Target %u\n",'A'+clocks->selected_dpll_id,ref_clk,use_ssc,dpll_tgt_freq);
		if(!find_ivb_dpll_dividers(dpll_tgt_freq,ref_clk,is_dp,clocks))return B_ERROR;
	}else{TRACE("Clocks: calc_display_clocks: Unsupp Gen %d\n",INTEL_DISPLAY_GEN(devInfo));return B_UNSUPPORTED;}
	if(clocks->needs_fdi){calculate_fdi_m_n_params(devInfo,pipe,clocks);} // Use pipe for BPC lookup
	return B_OK;
}


// --- DPLL Management Stubs (SKL+ and newer) ---

/**
 * Finds and reserves an available hardware DPLL for a given DDI port and frequency.
 * This is a complex function that needs to consider VBT data for port-DPLL compatibility,
 * current DPLL usage, and whether DPLLs can be shared.
 *
 * TODO: Full implementation requires detailed PRM lookup for each platform (SKL, KBL, CFL, ICL, TGL etc.)
 *       to understand DPLL capabilities, sharing rules, and VBT DDI-DPLL mapping.
 *
 * @param dev The i915 device private structure.
 * @param port_id The logical DDI port (_PRIV_PORT_A, _B, etc.) needing the clock.
 * @param target_pipe The logical pipe that will be driving this port.
 * @param required_freq_khz The target pixel clock or symbol clock required.
 * @param current_clock_params Provides context like if it's DP or HDMI.
 * @return Hardware DPLL ID (0 to MAX_HW_DPLLS-1) on success, negative error code on failure.
 */
int
i915_get_dpll_for_port(struct intel_i915_device_info* dev,
	enum intel_port_id_priv port_id, enum pipe_id_priv target_pipe,
	uint32_t required_freq_khz, const intel_clock_params_t* current_clock_params)
{
	TRACE("i915_get_dpll_for_port: Port %d, Pipe %d, Freq %u kHz (STUB)\n",
		port_id, target_pipe, required_freq_khz);

	// Extremely simplified stub: Try to find any unused DPLL.
	// Real logic would check VBT for port-to-DPLL mapping, frequency capabilities of DPLL,
	// and sharing possibilities.
	if (INTEL_GRAPHICS_GEN(dev->runtime_caps.device_id) >= 9) { // SKL+
		for (int i = 0; i < MAX_HW_DPLLS; i++) {
			if (!dev->dplls[i].is_in_use) {
				dev->dplls[i].is_in_use = true;
				dev->dplls[i].user_pipe = target_pipe;
				dev->dplls[i].user_port = port_id;
				dev->dplls[i].programmed_freq_khz = 0; // Mark as not yet programmed
				TRACE("i915_get_dpll_for_port: Assigning STUB DPLL ID %d to port %d\n", i, port_id);
				return i; // Return array index as DPLL ID
			}
		}
		ERROR("i915_get_dpll_for_port: No free DPLLs found (STUB logic)!\n");
		return -1; // B_NO_MEMORY or B_ERROR or custom error
	}
	// For older gens like HSW/IVB, DPLLs are more fixed per pipe, handled by existing logic.
	TRACE("i915_get_dpll_for_port: No SKL+ style DPLL management for this GEN (%d).\n", INTEL_GRAPHICS_GEN(dev->runtime_caps.device_id));
	return -1; // Or return a specific DPLL based on pipe for older gens if needed here.
}

/**
 * Releases a DPLL that was previously acquired by i915_get_dpll_for_port.
 */
void
i915_release_dpll(struct intel_i915_device_info* dev, int dpll_id, enum intel_port_id_priv port_id)
{
	TRACE("i915_release_dpll: Releasing DPLL ID %d (used by port %d) (STUB)\n", dpll_id, port_id);
	if (INTEL_GRAPHICS_GEN(dev->runtime_caps.device_id) >= 9) {
		if (dpll_id >= 0 && dpll_id < MAX_HW_DPLLS) {
			if (dev->dplls[dpll_id].is_in_use && dev->dplls[dpll_id].user_port == port_id) {
				dev->dplls[dpll_id].is_in_use = false;
				dev->dplls[dpll_id].user_pipe = PRIV_PIPE_INVALID;
				dev->dplls[dpll_id].user_port = PRIV_PORT_ID_NONE;
				dev->dplls[dpll_id].programmed_freq_khz = 0;
				// TODO: Potentially power down the hardware DPLL if no other port shares it.
			} else {
				ERROR("i915_release_dpll: Attempt to release DPLL %d not in use by port %d or invalid.\n", dpll_id, port_id);
			}
		} else {
			ERROR("i915_release_dpll: Invalid DPLL ID %d.\n", dpll_id);
		}
	}
}

/**
 * Programs a specific SKL+ style DPLL with given parameters.
 * TODO: Full implementation requires PRM details for SKL_DPLLx_CFGCR1, SKL_DPLLx_CFGCR2 registers.
 */
status_t
i915_program_skl_dpll(struct intel_i915_device_info* dev,
	int dpll_id, /* Hardware DPLL index: 0, 1, 2, 3 */
	const skl_dpll_params* params)
{
	TRACE("i915_program_skl_dpll: Programming DPLL ID %d (STUB)\n", dpll_id);
	if (INTEL_GRAPHICS_GEN(dev->runtime_caps.device_id) < 9)
		return B_UNSUPPORTED;
	if (dpll_id < 0 || dpll_id >= MAX_HW_DPLLS || params == NULL)
		return B_BAD_VALUE;

	// Example register access (actual registers are different for SKL DPLL0 vs DPLL1/2/3)
	// uint32_t cfgcr1_reg = SKL_DPLL_CFGCR1(dpll_id); // Macro needed
	// uint32_t cfgcr1_val = 0;
	// cfgcr1_val |= params->dco_integer;
	// cfgcr1_val |= (params->dco_fraction << SKL_DCO_FRACTION_SHIFT);
	// ... other params for CFGCR1 and CFGCR2 ...
	// intel_i915_write32(dev, cfgcr1_reg, cfgcr1_val);
	// intel_i915_write32(dev, SKL_DPLL_CFGCR2(dpll_id), ...);

	dev->dplls[dpll_id].programmed_freq_khz = 0; // TODO: Store actual freq based on params
	TRACE("TODO: Actual SKL DPLL %d programming logic needed using PRM.\n", dpll_id);
	return B_OK; // Placeholder
}

/**
 * Enables or disables a SKL+ style DPLL and configures its routing to a DDI port.
 * TODO: Full implementation requires PRM details for SKL_DPLL_ENABLE, SKL_DPLL_CTRL1/2 registers.
 */
status_t
i915_enable_skl_dpll(struct intel_i915_device_info* dev,
	int dpll_id, enum intel_port_id_priv port_id, bool enable)
{
	TRACE("i915_enable_skl_dpll: %s DPLL ID %d for Port %d (STUB)\n",
		enable ? "Enabling" : "Disabling", dpll_id, port_id);
	if (INTEL_GRAPHICS_GEN(dev->runtime_caps.device_id) < 9)
		return B_UNSUPPORTED;
	if (dpll_id < 0 || dpll_id >= MAX_HW_DPLLS)
		return B_BAD_VALUE;

	// 1. Program SKL_DPLL_CTRL1 to map this dpll_id to the target port_id's DDI.
	//    This involves reading SKL_DPLL_CTRL1, clearing bits for the target port,
	//    and setting new bits to link it to dpll_id.
	//    Example: port_id PRIV_PORT_A maps to DDI A.
	//    uint32_t ctrl1 = intel_i915_read32(dev, SKL_DPLL_CTRL1);
	//    ctrl1 &= ~SKL_DPLL_CTRL1_DDI_A_MASK; // Mask for DDI A's DPLL select
	//    if (enable) ctrl1 |= SKL_DPLL_CTRL1_DDI_A_MAP_DPLL(dpll_id);
	//    intel_i915_write32(dev, SKL_DPLL_CTRL1, ctrl1);

	// 2. Enable/Disable the DPLL itself via LCPLL_CTL like registers (e.g., DPLL_ENABLE(dpll_id))
	//    uint32_t dpll_enable_reg = DPLL_ENABLE_REG(dpll_id); // Macro needed
	//    uint32_t val = intel_i915_read32(dev, dpll_enable_reg);
	//    if (enable) val |= DPLL_POWER_ON_BIT | DPLL_PLL_ON_BIT;
	//    else val &= ~(DPLL_POWER_ON_BIT | DPLL_PLL_ON_BIT);
	//    intel_i915_write32(dev, dpll_enable_reg, val);
	//    if (enable) { /* ... poll for lock ... */ }

	TRACE("TODO: Actual SKL DPLL %d enable/disable and routing for port %d logic needed using PRM.\n", dpll_id, port_id);
	return B_OK; // Placeholder
}

// --- End DPLL Management Stubs ---


status_t
intel_i915_program_cdclk(intel_i915_device_info* devInfo, const intel_clock_params_t* clocks)
{
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	uint32_t target_cdclk_khz = clocks->cdclk_freq_khz;
	if (target_cdclk_khz == 0) { TRACE("Clocks: Program CDCLK target 0 kHz, skipping.\n"); return B_OK;}
	TRACE("Clocks: Programming CDCLK to target %u kHz.\n", target_cdclk_khz);

	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->hsw_cdclk_ctl_field_val == 0 && target_cdclk_khz != 0) {
			TRACE("Clocks: HSW: hsw_cdclk_ctl_field_val is 0, cannot program target CDCLK %u kHz.\n", target_cdclk_khz);
			return B_BAD_VALUE;
		}
		intel_i915_write32(devInfo, CDCLK_CTL_HSW, clocks->hsw_cdclk_ctl_field_val);
		devInfo->current_cdclk_freq_khz = target_cdclk_khz;
		TRACE("Clocks: HSW CDCLK_CTL programmed to 0x%08" B_PRIx32 " for target %u kHz.\n", clocks->hsw_cdclk_ctl_field_val, target_cdclk_khz);
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t cdclk_ctl_val = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
		uint32_t new_freq_sel_bits = 0;
		if (IS_IVYBRIDGE_MOBILE(devInfo->device_id)) {
			cdclk_ctl_val &= ~CDCLK_FREQ_SEL_IVB_MASK_MOBILE;
			// Mobile IVB has more granular CDCLK options.
			// These values (CDCLK_FREQ_xxx_MHZ_IVB_M) should map to actual hardware field values.
			if (target_cdclk_khz >= 675000) new_freq_sel_bits = CDCLK_FREQ_675_MHZ_IVB_M; // Field value for 675 MHz
			else if (target_cdclk_khz >= 540000) new_freq_sel_bits = CDCLK_FREQ_540_MHZ_IVB_M;   // Field value for 540 MHz
			else if (target_cdclk_khz >= 450000) new_freq_sel_bits = CDCLK_FREQ_450_MHZ_IVB_M;   // Field value for 450 MHz
			else if (target_cdclk_khz >= 337500) new_freq_sel_bits = CDCLK_FREQ_337_5_MHZ_IVB_M; // Field value for 337.5 MHz
			else {
				TRACE("Clocks: IVB Mobile: Target CDCLK %u kHz not directly mappable. Using closest supported or default.\n", target_cdclk_khz);
				// Default to a common safe value if no direct match or implement closest logic
				new_freq_sel_bits = CDCLK_FREQ_450_MHZ_IVB_M; // Example default
				target_cdclk_khz = 450000; // Reflect the actual frequency being set
			}
		} else { // IVB Desktop/Server
			cdclk_ctl_val &= ~CDCLK_FREQ_SEL_IVB_MASK_DESKTOP;
			// IVB Desktop/Server typically supports 400MHz (000b) and 320MHz (001b)
			// CDCLK_FREQ_400_IVB_D should be (0 << 0)
			// CDCLK_FREQ_320_IVB_D should be (1 << 0)
			// Assuming CDCLK_FREQ_SEL_IVB_MASK_DESKTOP is 0x7 (bits 2:0)
			if (target_cdclk_khz >= 400000) { // Prefer 400MHz if target is >= 400
				new_freq_sel_bits = CDCLK_FREQ_400_IVB_D; // Should be 0 for 400MHz (FSB/2)
				target_cdclk_khz = 400000;
			} else if (target_cdclk_khz >= 320000) { // Prefer 320MHz if target is >= 320
				new_freq_sel_bits = CDCLK_FREQ_320_IVB_D; // Should be 1 for 320MHz (FSB/2.5)
				target_cdclk_khz = 320000;
			} else {
				TRACE("Clocks: IVB Desktop: Target CDCLK %u kHz not mappable to 320/400. Defaulting to 400MHz.\n", target_cdclk_khz);
				new_freq_sel_bits = CDCLK_FREQ_400_IVB_D;
				target_cdclk_khz = 400000;
			}
		}
		cdclk_ctl_val |= new_freq_sel_bits;
		intel_i915_write32(devInfo, CDCLK_CTL_IVB, cdclk_ctl_val);
		devInfo->current_cdclk_freq_khz = target_cdclk_khz; // Store the actual frequency set
		TRACE("Clocks: IVB CDCLK_CTL programmed to 0x%08" B_PRIx32 " for target %u kHz (Mobile: %d).\n",
			cdclk_ctl_val, target_cdclk_khz, IS_IVYBRIDGE_MOBILE(devInfo->device_id));
	} else {
		TRACE("Clocks: intel_i915_program_cdclk: Unsupported generation.\n"); return B_UNSUPPORTED;
	}
	snooze(1000);
	(void)intel_i915_read32(devInfo, IS_HASWELL(devInfo->device_id) ? CDCLK_CTL_HSW : CDCLK_CTL_IVB);
	return B_OK;
}

status_t
intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, const intel_clock_params_t* clocks)
{
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	status_t status = B_OK;

	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->is_wrpll) {
			int dpll_idx = clocks->selected_dpll_id;
			if (dpll_idx < 0 || dpll_idx > 1) { return B_BAD_INDEX; }
			uint32_t wrpll_ctl_val = intel_i915_read32(devInfo, WRPLL_CTL(dpll_idx));
			wrpll_ctl_val &= ~(WRPLL_REF_LCPLL_HSW | WRPLL_REF_SSC_HSW | (0x7U << WRPLL_DP_LINKRATE_SHIFT_HSW));
			if (clocks->lcpll_freq_khz > 0) wrpll_ctl_val |= WRPLL_REF_LCPLL_HSW;
			else wrpll_ctl_val |= WRPLL_REF_SSC_HSW;
			if (clocks->is_dp_or_edp) {
				if (clocks->dp_link_rate_khz >= 540000) wrpll_ctl_val |= WRPLL_DP_LINKRATE_5_4;
				else if (clocks->dp_link_rate_khz >= 270000) wrpll_ctl_val |= WRPLL_DP_LINKRATE_2_7;
				else wrpll_ctl_val |= WRPLL_DP_LINKRATE_1_62;
			}
			uint32_t wrpll_div_frac_reg = WRPLL_DIV_FRAC_REG_HSW(dpll_idx);
			uint32_t wrpll_target_count_reg = WRPLL_TARGET_COUNT_REG_HSW(dpll_idx);
			uint32_t div_frac_val = 0; uint32_t target_count_val = 0;
			if (clocks->wrpll_m2_frac_en && clocks->wrpll_m2_frac > 0) {
				div_frac_val |= HSW_WRPLL_M2_FRAC_ENABLE;
				div_frac_val |= (clocks->wrpll_m2_frac << HSW_WRPLL_M2_FRAC_SHIFT) & HSW_WRPLL_M2_FRAC_MASK;
			}
			div_frac_val |= (clocks->wrpll_m2 << HSW_WRPLL_M2_INT_SHIFT) & HSW_WRPLL_M2_INT_MASK;
			div_frac_val |= (((clocks->wrpll_n - 2)) << HSW_WRPLL_N_DIV_SHIFT) & HSW_WRPLL_N_DIV_MASK;
			target_count_val |= (clocks->wrpll_p1 << HSW_WRPLL_P1_DIV_SHIFT) & HSW_WRPLL_P1_DIV_MASK;
			target_count_val |= (clocks->wrpll_p2 << HSW_WRPLL_P2_DIV_SHIFT) & HSW_WRPLL_P2_DIV_MASK;
			intel_i915_write32(devInfo, wrpll_div_frac_reg, div_frac_val);
			intel_i915_write32(devInfo, wrpll_target_count_reg, target_count_val);
			intel_i915_write32(devInfo, WRPLL_CTL(dpll_idx), wrpll_ctl_val);
			TRACE("HSW WRPLL Prog: CTL(idx %d)=0x%08" B_PRIx32 ", DIV_FRAC=0x%08" B_PRIx32 ", TGT_COUNT=0x%08" B_PRIx32 "\n",
				dpll_idx, wrpll_ctl_val, div_frac_val, target_count_val);
		} else { // SPLL
			uint32_t spll_ctl_val = intel_i915_read32(devInfo, SPLL_CTL_HSW);
			spll_ctl_val &= (SPLL_PLL_ENABLE_HSW | SPLL_PLL_OVERRIDE_HSW | SPLL_PCH_SSC_ENABLE_HSW);
			spll_ctl_val |= SPLL_REF_LCPLL_HSW; // Assuming LCPLL ref for now
			spll_ctl_val |= (clocks->spll_m2 << SPLL_M2_INT_SHIFT_HSW) & SPLL_M2_INT_MASK_HSW;
			spll_ctl_val |= (clocks->spll_n << SPLL_N_SHIFT_HSW) & SPLL_N_MASK_HSW;
			spll_ctl_val |= (clocks->spll_p1 << SPLL_P1_SHIFT_HSW) & SPLL_P1_MASK_HSW;
			spll_ctl_val |= (clocks->spll_p2 << SPLL_P2_SHIFT_HSW) & SPLL_P2_MASK_HSW;
			intel_i915_write32(devInfo, SPLL_CTL_HSW, spll_ctl_val);
			TRACE("HSW SPLL_CTL set to 0x%08" B_PRIx32 "\n", spll_ctl_val);
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t dpll_reg = (clocks->selected_dpll_id == DPLL_ID_DPLL_A_IVB) ? DPLL_A_IVB : DPLL_B_IVB;
		uint32_t dpll_md_reg = (clocks->selected_dpll_id == DPLL_ID_DPLL_A_IVB) ? DPLL_MD_A_IVB : DPLL_MD_B_IVB;
		uint32_t dpll_val = intel_i915_read32(devInfo, dpll_reg);
		dpll_val &= (DPLL_VCO_ENABLE_IVB | DPLL_OVERRIDE_IVB | DPLL_PORT_TRANS_SELECT_IVB_MASK | DPLL_REF_CLK_SEL_IVB_MASK);
		dpll_val |= (clocks->wrpll_p1 << DPLL_FPA0_P1_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P1_POST_DIV_MASK_IVB;
		dpll_val |= (clocks->wrpll_n << DPLL_FPA0_N_DIV_SHIFT_IVB) & DPLL_FPA0_N_DIV_MASK_IVB;
		dpll_val |= (clocks->ivb_dpll_m1_reg_val << DPLL_FPA0_M1_DIV_SHIFT_IVB) & DPLL_FPA0_M1_DIV_MASK_IVB;
		dpll_val |= (clocks->wrpll_m2 << DPLL_FPA0_M2_DIV_SHIFT_IVB) & DPLL_FPA0_M2_DIV_MASK_IVB;
		dpll_val &= ~(DPLL_MODE_MASK_IVB | DPLL_FPA0_P2_POST_DIV_MASK_IVB);
		if (clocks->is_dp_or_edp) dpll_val |= DPLL_MODE_DP_IVB | ((clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB);
		else if (clocks->is_lvds) dpll_val |= DPLL_MODE_LVDS_IVB | ((clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB);
		else dpll_val |= DPLL_MODE_HDMI_DVI_IVB | ((clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB);
		uint32_t dpll_md_val = (clocks->is_dp_or_edp && clocks->pixel_multiplier > 0) ? ((clocks->pixel_multiplier - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB) : 0;
		intel_i915_write32(devInfo, dpll_reg, dpll_val);
		intel_i915_write32(devInfo, dpll_md_reg, dpll_md_val);
		TRACE("IVB DPLL programming: DPLL_VAL=0x%08" B_PRIx32 ", DPLL_MD_VAL=0x%08" B_PRIx32 "\n", dpll_val, dpll_md_val);
	} else {
		status = B_UNSUPPORTED; TRACE("program_dpll_for_pipe: Unsupported GEN\n");
	}
	return status;
}

status_t
intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks)
{
	TRACE("enable_dpll for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	uint32_t reg_ctl, val, enable_bit, lock_bit;
	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->is_wrpll) { reg_ctl = WRPLL_CTL(clocks->selected_dpll_id); enable_bit = WRPLL_PLL_ENABLE; lock_bit = WRPLL_PLL_LOCK;}
		else { reg_ctl = SPLL_CTL_HSW; enable_bit = SPLL_PLL_ENABLE_HSW; lock_bit = SPLL_PLL_LOCK_HSW;}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		reg_ctl = (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) ? DPLL_A_IVB : DPLL_B_IVB;
		enable_bit = DPLL_VCO_ENABLE_IVB; lock_bit = DPLL_LOCK_IVB;
		TRACE("IVB enable_dpll using DPLL_A/B_IVB (Reg 0x%x)\n", reg_ctl);
	} else { TRACE("enable_dpll: Unsupported device generation.\n"); return B_ERROR;}

	val = intel_i915_read32(devInfo, reg_ctl);
	if (enable) val |= enable_bit; else val &= ~enable_bit;
	intel_i915_write32(devInfo, reg_ctl, val); (void)intel_i915_read32(devInfo, reg_ctl);
	if (enable) {
		snooze(20); bigtime_t startTime = system_time();
		while (system_time() - startTime < 5000) {
			if (intel_i915_read32(devInfo, reg_ctl) & lock_bit) {
				TRACE("DPLL (Reg 0x%x) locked.\n", reg_ctl); return B_OK;
			}
			snooze(100);
		}
		TRACE("DPLL (Reg 0x%x) lock TIMEOUT. Value: 0x%08" B_PRIx32 "\n", reg_ctl, intel_i915_read32(devInfo, reg_ctl));
		val &= ~enable_bit; intel_i915_write32(devInfo, reg_ctl, val); return B_TIMED_OUT;
	}
	return B_OK;
}

status_t intel_i915_program_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks)
{
	TRACE("FDI: Program FDI for pipe %d\n", pipe);
	if (!clocks || !clocks->needs_fdi || !(IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id))) {
		return B_OK;
	}

	uint16_t data_m = clocks->fdi_params.data_m;
	uint16_t data_n = clocks->fdi_params.data_n;
	uint16_t link_m = clocks->fdi_params.link_m;
	uint16_t link_n = clocks->fdi_params.link_n;
	uint8_t fdi_lanes = clocks->fdi_params.fdi_lanes;
	uint16_t tu_size = clocks->fdi_params.tu_size;

	if (fdi_lanes == 0) fdi_lanes = 2;
	if (tu_size == 0) tu_size = 64;

	if (data_m == 0 || data_n == 0) {
		TRACE("FDI: M/N values are zero for pipe %d. Using placeholders. Calculation might have failed.\n", pipe);
		data_m = 22; data_n = 24; link_m = data_m; link_n = data_n;
	}

	intel_i915_write32(devInfo, FDI_TX_MVAL_IVB_REG(pipe), FDI_MVAL_TU_SIZE(tu_size) | data_m);
	intel_i915_write32(devInfo, FDI_TX_NVAL_IVB_REG(pipe), data_n);
	intel_i915_write32(devInfo, FDI_RX_MVAL_IVB_REG(pipe), FDI_MVAL_TU_SIZE(tu_size) | link_m);
	intel_i915_write32(devInfo, FDI_RX_NVAL_IVB_REG(pipe), link_n);
	TRACE("FDI Programmed: Pipe %d, TX_MVAL=0x%x (TU %u, M %u), TX_NVAL=%u, RX_MVAL=0x%x (TU %u, M %u), RX_NVAL=%u\n",
		pipe, intel_i915_read32(devInfo, FDI_TX_MVAL_IVB_REG(pipe)), tu_size, data_m, data_n,
		intel_i915_read32(devInfo, FDI_RX_MVAL_IVB_REG(pipe)), tu_size, link_m, link_n);


	uint32_t fdi_tx_ctl_reg = FDI_TX_CTL(pipe);
	uint32_t fdi_rx_ctl_reg = FDI_RX_CTL(pipe);
	uint32_t fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
	uint32_t fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);
	fdi_tx_val &= (FDI_TX_ENABLE | FDI_PCDCLK_CHG_STATUS_IVB);
	fdi_rx_val &= (FDI_RX_ENABLE | FDI_RX_PLL_ENABLE_IVB | FDI_FS_ERRC_ENABLE_IVB | FDI_FE_ERRC_ENABLE_IVB);
	fdi_tx_val &= ~FDI_TX_CTL_LANE_MASK_IVB;
	if (fdi_lanes == 1) fdi_tx_val |= FDI_TX_CTL_LANE_1_IVB;
	else if (fdi_lanes == 2) fdi_tx_val |= FDI_TX_CTL_LANE_2_IVB;
	else if (fdi_lanes == 3 && IS_IVYBRIDGE(devInfo->device_id)) fdi_tx_val |= FDI_TX_CTL_LANE_3_IVB;
	else if (fdi_lanes == 4) fdi_tx_val |= FDI_TX_CTL_LANE_4_IVB;
	else fdi_tx_val |= FDI_TX_CTL_LANE_2_IVB;
	fdi_tx_val &= ~FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB;
	fdi_tx_val |= FDI_LINK_TRAIN_PATTERN_1_IVB;
	fdi_tx_val &= ~(FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB | FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB);
	fdi_tx_val |= FDI_TX_CTL_VOLTAGE_SWING_LEVEL_0_IVB; // Start with VS0
	fdi_tx_val |= FDI_TX_CTL_PRE_EMPHASIS_LEVEL_0_IVB;   // Start with PE0
	fdi_rx_val &= ~FDI_RX_CTL_LANE_MASK_IVB;
	if (fdi_lanes == 1) fdi_rx_val |= FDI_RX_CTL_LANE_1_IVB;
	else if (fdi_lanes == 2) fdi_rx_val |= FDI_RX_CTL_LANE_2_IVB;
	else if (fdi_lanes == 3 && IS_IVYBRIDGE(devInfo->device_id)) fdi_rx_val |= FDI_RX_CTL_LANE_3_IVB;
	else if (fdi_lanes == 4) fdi_rx_val |= FDI_RX_CTL_LANE_4_IVB;
	else fdi_rx_val |= FDI_RX_CTL_LANE_2_IVB;
	intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
	intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
	TRACE("FDI: Programmed FDI_TX_CTL(pipe %d)=0x%08x, FDI_RX_CTL(pipe %d)=0x%08x\n", pipe, fdi_tx_val, pipe, fdi_rx_val);
	return B_OK;
}

status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	TRACE("FDI: Enable FDI for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	if (!(IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id))) {
		return B_OK;
	}
	uint32_t fdi_tx_ctl_reg = FDI_TX_CTL(pipe);
	uint32_t fdi_rx_ctl_reg = FDI_RX_CTL(pipe);
	uint32_t fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
	uint32_t fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);

	if (enable) {
		fdi_rx_val |= FDI_RX_PLL_ENABLE_IVB;
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg); snooze(10);
		fdi_tx_val |= FDI_TX_ENABLE; fdi_rx_val |= FDI_RX_ENABLE;
		intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg);
		int retries = 0; bool fdi_trained = false; uint32_t fdi_rx_iir_reg = FDI_RX_IIR(pipe);
		uint32_t current_vs_level_idx = 0; // Index for VS: 0 (0.4V), 1 (0.6V), 2 (0.8V)
		uint32_t current_pe_level_idx = 0; // Index for PE: 0 (0dB), 1 (3.5dB)
		const uint32_t vs_field_map[] = {FDI_TX_CTL_VOLTAGE_SWING_LEVEL_0_IVB, FDI_TX_CTL_VOLTAGE_SWING_LEVEL_1_IVB, FDI_TX_CTL_VOLTAGE_SWING_LEVEL_2_IVB};
		const uint32_t pe_field_map[] = {FDI_TX_CTL_PRE_EMPHASIS_LEVEL_0_IVB, FDI_TX_CTL_PRE_EMPHASIS_LEVEL_1_IVB, FDI_TX_CTL_PRE_EMPHASIS_LEVEL_2_IVB};
		const int max_vs_idx = sizeof(vs_field_map)/sizeof(vs_field_map[0]) -1;
		const int max_pe_idx = sizeof(pe_field_map)/sizeof(pe_field_map[0]) -1;

		while (retries < ( (max_vs_idx + 1) * (max_pe_idx + 1) ) ) { // Max attempts based on VS/PE combos
			if (retries > 0) { // Not the first attempt
				current_pe_level_idx++;
				if (current_pe_level_idx > max_pe_idx) {
					current_pe_level_idx = 0;
					current_vs_level_idx++;
					if (current_vs_level_idx > max_vs_idx) {
						TRACE("FDI: Exhausted all VS/PE combinations for pipe %d.\n", pipe);
						break; // Exhausted all combos
					}
				}
				fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
				fdi_tx_val &= ~(FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB | FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB);
				fdi_tx_val |= vs_field_map[current_vs_level_idx] | pe_field_map[current_pe_level_idx];
				fdi_tx_val &= ~FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB; // Ensure pattern 1
				fdi_tx_val |= FDI_LINK_TRAIN_PATTERN_1_IVB | FDI_TX_ENABLE; // Ensure enabled
				intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
				TRACE("FDI: Link training attempt %d for pipe %d. Trying VS Idx %u, PE Idx %u (RegVal 0x%x)\n",
					retries + 1, pipe, current_vs_level_idx, current_pe_level_idx, fdi_tx_val);
			}

			snooze(1000); uint32_t iir_val = intel_i915_read32(devInfo, fdi_rx_iir_reg);
			if (iir_val & FDI_RX_BIT_LOCK_IVB) {
				fdi_trained = true; TRACE("FDI: Link training for pipe %d successful (bit lock achieved).\n", pipe);
				intel_i915_write32(devInfo, fdi_rx_iir_reg, iir_val & FDI_RX_BIT_LOCK_IVB); break;
			}
			TRACE("FDI: Link training poll %d for pipe %d, IIR=0x%08x (BitLock not set).\n", retries + 1, pipe, iir_val);
			retries++;
		}

		if (fdi_trained) {
			fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
			fdi_tx_val &= ~FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB; fdi_tx_val |= FDI_LINK_TRAIN_NONE_IVB;
			intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
			TRACE("FDI: Link training for pipe %d complete, pattern set to NONE.\n", pipe);
		} else {
			TRACE("FDI: Link training for pipe %d FAILED after all retries.\n", pipe);
			fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
			fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);
			fdi_tx_val &= ~(FDI_TX_ENABLE | FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB);
			fdi_rx_val &= ~(FDI_RX_ENABLE | FDI_RX_PLL_ENABLE_IVB);
			intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
			intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
			return B_ERROR;
		}
	} else {
		fdi_tx_val &= ~(FDI_TX_ENABLE | FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB);
		fdi_rx_val &= ~(FDI_RX_ENABLE | FDI_RX_PLL_ENABLE_IVB);
		intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		TRACE("FDI: Disabling FDI TX/RX and RX PLL for pipe %d.\n", pipe);
	}
	(void)intel_i915_read32(devInfo, fdi_tx_ctl_reg);
	(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg);
	return B_OK;
}
