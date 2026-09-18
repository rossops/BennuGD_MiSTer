//============================================================================
//
//  BennuGD for MiSTer: the FPGA half of a Linux/ARM game runtime.
//
//  The HPS runs the BennuGD interpreter (see hps/) and writes RGB565 frames
//  into DDR3. rtl/bennugd_video.sv reads them back a row ahead of the beam
//  and drives the core's video outputs with a 240p / 15.7 kHz timing, so the
//  picture reaches HDMI through the scaler like any other core's, the analog
//  outputs directly, and direct_video works. The scaler-framebuffer path
//  (MISTER_FB) is kept behind an OSD switch as a fallback for HDMI.
//  Audio reaches the DAC/HDMI through the framework's alsa.sv from
//  /dev/MrAudio, so the core's own audio outputs stay silent. Controllers
//  come through hps_io's joystick words, handed to the HPS in the control
//  block (rtl/bennugd_ctl.sv).
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or (at your option)
//  any later version.
//
//============================================================================

module emu
(
	`include "sys/emu_ports.vh"
);

///////// Default values for ports not used in this core /////////

assign ADC_BUS  = 'Z;
assign USER_OUT = '1;
assign {UART_RTS, UART_TXD, UART_DTR} = 0;
assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
assign {SDRAM_DQ, SDRAM_A, SDRAM_BA, SDRAM_CLK, SDRAM_CKE, SDRAM_DQML, SDRAM_DQMH, SDRAM_nWE, SDRAM_nCAS, SDRAM_nRAS, SDRAM_nCS} = 'Z;

assign VGA_SL = 0;
assign VGA_F1 = 0;
assign VGA_SCALER  = 0;
assign VGA_DISABLE = 0;
assign HDMI_FREEZE = 0;
assign HDMI_BLACKOUT = 0;
assign HDMI_BOB_DEINT = 0;

// silent: HPS audio comes in through sys/alsa.sv (MISTER_DISABLE_ALSA must stay off)
assign AUDIO_S = 1;
assign AUDIO_L = 0;
assign AUDIO_R = 0;
assign AUDIO_MIX = 0;

assign LED_DISK = 0;
assign LED_POWER = 0;
assign BUTTONS = 0;

///////// HPS framebuffer and control block (addresses shared with hps/frontend) /////////

// Two RGB565 buffers at 0x22000000 and 0x22100000 (above ascal's own buffers
// at 0x20000000), a 64-byte control block at 0x23F00000. The HPS picks the
// buffer and the geometry through the block; defaults are SorR's 416x240.
// FB_EN makes the scaler show that buffer directly instead of the core's
// video (HDMI only); off by default, see the OSD option.
assign FB_EN          = status[3];
assign FB_FORMAT      = 5'b10100;       // 16bpp 565 with bit 4 (BGR) set: this is how RGB565 in memory
                                        // comes out with red as red (bit 4 clear swapped R and B on screen)
assign FB_FORCE_BLANK = 0;

assign DDRAM_CLK = CLK_VIDEO;

// hps_io joystick words are in the clk_sys domain; they change at human speed,
// so two registers are all the crossing they need
reg [31:0] joy0_v, joy1_v, joy2_v, joy3_v;
always @(posedge CLK_VIDEO) begin
	joy0_v <= joystick_0; joy1_v <= joystick_1; joy2_v <= joystick_2; joy3_v <= joystick_3;
end

wire        line_req, line_buf, line_we;
wire [11:0] line_y;
wire  [8:0] line_waddr;
wire [63:0] line_wdata;
wire        hs, vs, hblank, vblank;   // from bennugd_video below

bennugd_ctl ctl
(
	.clk(CLK_VIDEO),
	.reset(RESET),
	.vs(vs),                // the control block counts vertical syncs
	.joy0(joy0_v), .joy1(joy1_v), .joy2(joy2_v), .joy3(joy3_v),

	.fb_base(FB_BASE),
	.fb_width(FB_WIDTH),
	.fb_height(FB_HEIGHT),
	.fb_stride(FB_STRIDE),

	.line_req(line_req),
	.line_y(line_y),
	.line_buf(line_buf),
	.line_we(line_we),
	.line_waddr(line_waddr),
	.line_wdata(line_wdata),

	.ddr_rd(DDRAM_RD),
	.ddr_we(DDRAM_WE),
	.ddr_burst(DDRAM_BURSTCNT),
	.ddr_addr(DDRAM_ADDR),
	.ddr_din(DDRAM_DIN),
	.ddr_be(DDRAM_BE),
	.ddr_dout(DDRAM_DOUT),
	.ddr_dout_ready(DDRAM_DOUT_READY),
	.ddr_busy(DDRAM_BUSY)
);

///////// OSD /////////

// "Original" keeps the framebuffer's own pixel aspect (416:240 for SorR)
wire [1:0] ar = status[122:121];
assign VIDEO_ARX = (!ar) ? FB_WIDTH  : (ar - 1'd1);
assign VIDEO_ARY = (!ar) ? FB_HEIGHT : 12'd0;

`include "build_id.v"
localparam CONF_STR = {
	"BennuGD;;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"-;",
	// An SD-image slot: selecting a .dat here (or through an .mgl) makes Main
	// mount the file, which costs nothing, and record its path in
	// /tmp/FULLPATH for bennugd-launcherd. The core never reads the image.
	"S0,DATDCB,Load game;",
	"-;",
	"J1,A,B,X,Y,L,R,Select,Start;",   // joystick word bits 4..11, read by the HPS through the control block
	"jn,A,B,X,Y,L,R,Select,Start;",
	"-;",
	"O[3],HDMI picture,Core video,HPS framebuffer;",   // fallback: the scaler reads DDR3 itself
	"V,v",`BUILD_DATE
};

wire forced_scandoubler;
wire [127:0] status;
wire  [31:0] joystick_0, joystick_1, joystick_2, joystick_3;

hps_io #(.CONF_STR(CONF_STR)) hps_io
(
	.clk_sys(clk_sys),
	.HPS_BUS(HPS_BUS),
	.EXT_BUS(),
	.gamma_bus(),
	.forced_scandoubler(forced_scandoubler),
	.buttons(),
	.status(status),
	.status_menumask(0),
	.joystick_0(joystick_0),
	.joystick_1(joystick_1),
	.joystick_2(joystick_2),
	.joystick_3(joystick_3),
	.ps2_key()
);

///////// Clocks /////////

wire clk_sys;
pll pll
(
	.refclk(CLK_50M),
	.rst(0),
	.outclk_0(clk_sys),     // 100 MHz
	.outclk_1()             // 20 MHz, unused
);
assign CLK_VIDEO = clk_sys;

///////// Native video: the framebuffer read back as 240p / 15.7 kHz /////////

bennugd_video video
(
	.clk(CLK_VIDEO),
	.reset(RESET),
	.scandouble(forced_scandoubler),
	.fb_width(FB_WIDTH),
	.fb_height(FB_HEIGHT),
	.line_req(line_req),
	.line_y(line_y),
	.line_buf(line_buf),
	.line_we(line_we),
	.line_waddr(line_waddr),
	.line_wdata(line_wdata),
	.ce_pix(CE_PIXEL),
	.hs(hs),
	.vs(vs),
	.hblank(hblank),
	.vblank(vblank),
	.r(VGA_R),
	.g(VGA_G),
	.b(VGA_B)
);

assign VGA_HS = hs;
assign VGA_VS = vs;
assign VGA_DE = ~(hblank | vblank);

///////// Heartbeat /////////

reg [26:0] act_cnt;
always @(posedge clk_sys) act_cnt <= act_cnt + 1'd1;
assign LED_USER = act_cnt[26] ? act_cnt[25:18] > act_cnt[7:0] : act_cnt[25:18] <= act_cnt[7:0];

endmodule
