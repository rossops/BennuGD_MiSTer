// Testbench for bennugd_ctl + bennugd_video against a model of the DDRAM
// port: pixel (x, y) of the framebuffer reads {y[7:0], x[7:0]}. Checks the
// active area size, every pixel value, and the line/frame periods, for
// three geometries in progressive mode and one scandoubled.
//   tools/sim.sh   (iverilog)
`timescale 1ns/1ps

module tb_video;

reg clk = 0;
always #5 clk = ~clk;   // 100 MHz

reg reset = 1;
reg scandouble = 0;
reg [11:0] cfg_w = 416, cfg_h = 240;   // what the "HPS" writes into the control block
reg [13:0] cfg_stride = 832;

// ---- DUT wiring ----
wire [31:0] fb_base; wire [11:0] fb_width, fb_height; wire [13:0] fb_stride;
wire line_req, line_buf; wire [11:0] line_y;
wire line_we; wire [8:0] line_waddr; wire [63:0] line_wdata;
wire ddr_rd, ddr_we; wire [7:0] ddr_burst; wire [28:0] ddr_addr; wire [63:0] ddr_din; wire [7:0] ddr_be;
reg  [63:0] ddr_dout = 0; reg ddr_dout_ready = 0; reg ddr_busy = 0;
wire ce_pix, hs, vs, hblank, vblank; wire [7:0] r, g, b;

bennugd_ctl ctl (
	.clk(clk), .reset(reset), .vs(vs), .joy0(32'h11), .joy1(32'h22), .joy2(32'h33), .joy3(32'h44),
	.fb_base(fb_base), .fb_width(fb_width), .fb_height(fb_height), .fb_stride(fb_stride),
	.line_req(line_req), .line_y(line_y), .line_buf(line_buf),
	.line_we(line_we), .line_waddr(line_waddr), .line_wdata(line_wdata),
	.ddr_rd(ddr_rd), .ddr_we(ddr_we), .ddr_burst(ddr_burst), .ddr_addr(ddr_addr), .ddr_din(ddr_din), .ddr_be(ddr_be),
	.ddr_dout(ddr_dout), .ddr_dout_ready(ddr_dout_ready), .ddr_busy(ddr_busy));

bennugd_video video (
	.clk(clk), .reset(reset), .scandouble(scandouble),
	.fb_width(fb_width), .fb_height(fb_height),
	.line_req(line_req), .line_y(line_y), .line_buf(line_buf),
	.line_we(line_we), .line_waddr(line_waddr), .line_wdata(line_wdata),
	.ce_pix(ce_pix), .hs(hs), .vs(vs), .hblank(hblank), .vblank(vblank), .r(r), .g(g), .b(b));

// ---- DDRAM model: reads answered after a latency, one beat per cycle ----
localparam [31:0] CTL = 32'h23F00000, FB0 = 32'h22000000, FB1 = 32'h22100000;
function [15:0] pixel(input [31:0] byte_addr);
	reg [31:0] off; reg [31:0] y, x;
	begin
		off = byte_addr - (byte_addr >= FB1 ? FB1 : FB0);
		y = off / cfg_stride; x = (off % cfg_stride) / 2;
		pixel = {y[7:0], x[7:0]};
	end
endfunction
function [63:0] word(input [28:0] a);
	reg [31:0] ba;
	begin
		ba = {a, 3'b0};
		if (ba == CTL)          word = {20'd0, cfg_w, 31'd0, 1'b0};        // present 0, width
		else if (ba == CTL + 8) word = {18'd0, cfg_stride, 20'd0, cfg_h};   // height, stride
		else if (ba >= FB0 && ba < FB1 + 32'h100000)
			word = {pixel(ba + 6), pixel(ba + 4), pixel(ba + 2), pixel(ba)};
		else word = 64'hDEADBEEF_DEADBEEF;
	end
endfunction

integer reads = 0, writes = 0, k;
reg [7:0] nbeats; reg [28:0] raddr;
always @(posedge clk) begin
	ddr_dout_ready <= 0;
	if (ddr_rd && !ddr_busy) begin
		ddr_busy <= 1; nbeats = ddr_burst; raddr = ddr_addr; reads = reads + 1;
		repeat (12) @(posedge clk);                       // latency
		for (k = 0; k < nbeats; k = k + 1) begin
			ddr_dout <= word(raddr + k); ddr_dout_ready <= 1;
			@(posedge clk);
		end
		ddr_dout_ready <= 0; ddr_busy <= 0;
	end
	else if (ddr_we && !ddr_busy) begin
		writes = writes + 1;
		ddr_busy <= 1; repeat (4) @(posedge clk); ddr_busy <= 0;
	end
end

// ---- checker, sampled on ce_pix like the scaler does ----
wire de = ~(hblank | vblank);
integer px_in_line = 0, lines_in_frame = 0, errors = 0, frames = 0;
integer col = 0, row = 0, line_started = 0;
integer hs_t = -1, vs_t = -1, line_cycles = 0, frame_lines = 0;
integer exp_w, exp_h, exp_step, exp_pixels_ok = 0;
reg de_d = 0, hs_d = 0, vs_d = 0;
reg checking = 0;   // the geometry takes two frames to reach the display after reset
reg [15:0] want;
always @(posedge clk) if (ce_pix) begin
	de_d <= de; hs_d <= hs; vs_d <= vs;
	if (de && checking) begin
		if (!de_d) begin col = 0; line_started = 1; end
		// r/g/b carry pixel (col, row); rows repeat when scandoubled
		want = {row[7:0], col[7:0]};
		if (r[7:3] !== want[15:11] || g[7:2] !== want[10:5] || b[7:3] !== want[4:0]) begin
			if (errors < 10) $display("  pixel mismatch frame %0d row %0d col %0d: got %02x %02x %02x want %04x",
				frames, row, col, r, g, b, want);
			errors = errors + 1;
		end
		else exp_pixels_ok = exp_pixels_ok + 1;
		col = col + 1;
	end
	else if (de_d && checking) begin
		if (col != exp_w) begin $display("  line %0d has %0d pixels, want %0d", row, col, exp_w); errors = errors + 1; end
		row = row + (scandouble ? (lines_in_frame[0] ? exp_step : 0) : exp_step);
		lines_in_frame = lines_in_frame + 1;
	end
	if (hs && !hs_d) begin
		if (hs_t >= 0) line_cycles = ($time - hs_t) / 10;
		hs_t = $time; frame_lines = frame_lines + 1;
	end
	if (vs && !vs_d) begin
		if (vs_t >= 0) begin
			frames = frames + 1;
			$display("frame %0d: %0d active lines (want %0d), %0d lines/frame, %0d cycles/line, %0d ok pixels, errors %0d",
				frames, lines_in_frame, scandouble ? exp_h * 2 : exp_h, frame_lines, line_cycles, exp_pixels_ok, errors);
			if (checking) begin
				if (lines_in_frame != (scandouble ? exp_h * 2 : exp_h)) errors = errors + 1;
				if (frame_lines != (scandouble ? 524 : 262)) errors = errors + 1;
				if (line_cycles != (scandouble ? 3180 : 6360)) errors = errors + 1;
			end
			checking = (frames >= 2);
		end
		vs_t = $time; lines_in_frame = 0; frame_lines = 0; row = 0; exp_pixels_ok = 0;
	end
end

task run_case(input [11:0] w, input [11:0] h, input sd, input [79:0] name);
	begin
		$display("== %0s: %0dx%0d scandouble=%0d", name, w, h, sd);
		cfg_w = w; cfg_h = h; cfg_stride = (w * 2 + 63) & ~63; scandouble = sd;
		exp_w = w; exp_step = (h > 262) ? 2 : 1; exp_h = (h > 262) ? h / 2 : h;
		reset = 1; repeat (10) @(posedge clk); reset = 0;
		vs_t = -1; hs_t = -1; frames = 0; checking = 0;
		// frames 3 and 4 are checked: the geometry takes two frames to reach the display
		wait (frames == 4);
	end
endtask

initial begin
	run_case(416, 240, 0, "SorR");
	run_case(320, 200, 0, "320x200");
	run_case(832, 480, 0, "832x480");
	run_case(416, 240, 1, "SorR 31k");
	$display("%s: %0d errors, %0d DDR reads, %0d writes", errors ? "FAIL" : "PASS", errors, reads, writes);
	$finish;
end

endmodule
