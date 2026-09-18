//============================================================================
//  BennuGD native video: a 240p / 15.7 kHz timing whose active area is the
//  HPS framebuffer, read from DDR3 one row ahead of the beam.
//
//  The counters run on the 100 MHz clock. A line is 6360 cycles (63.6 us),
//  262 lines make a 60.0 Hz frame. Pixels are emitted every `div` cycles,
//  with `div` chosen from the frame width so that the picture fills about
//  52 us of the line whatever the game renders (416 px -> 12 cycles,
//  320 px -> 16): a 4:3 game fills a 4:3 screen, SorR's widescreen mode
//  fills it too. With the framework's forced scandoubler every horizontal
//  figure halves, the frame has 524 lines and each row is shown twice, the
//  way the Menu core does it.
//
//  Rows: at the start of every output line the row for the next line is
//  requested from bennugd_ctl into the other half of a two-row line
//  buffer (four 16-bit banks written 64 bits at a time, so a pixel is one
//  read of the bank its x selects). Frames taller than the timing
//  (SorR's 832x480 modes) show every other row; narrower or shorter frames
//  are centred.
//============================================================================

module bennugd_video
(
	input             clk,            // 100 MHz, also the DDRAM clock
	input             reset,
	input             scandouble,     // hps_io forced_scandoubler

	input      [11:0] fb_width,       // from bennugd_ctl, change at vblank
	input      [11:0] fb_height,

	output reg        line_req,       // one-cycle: fetch row line_y into half line_buf
	output reg [11:0] line_y,
	output reg        line_buf,
	input             line_we,
	input       [8:0] line_waddr,
	input      [63:0] line_wdata,

	output            ce_pix,
	output reg        hs, vs,         // active-high pulses, as sys/ expects from a core
	output reg        hblank, vblank,
	output reg  [7:0] r, g, b
);

// ----- constants, 100 MHz cycles (progressive); halved when scandoubled -----
localparam H_TOTAL = 13'd6360;   // 63.6 us
localparam H_SYNC  = 13'd470;    // 4.7 us
localparam H_VIS0  = 13'd1040;   // nominal visible area starts 10.4 us in
localparam H_VISN  = 13'd5170;   // and lasts 51.7 us
localparam V_ACT   = 10'd240;    // rows 0..239 carry the picture
localparam V_TOTAL = 10'd262;
localparam V_SYNC0 = 10'd245;
localparam V_SYNCN = 10'd3;

wire [12:0] h_total = scandouble ? H_TOTAL >> 1 : H_TOTAL;
wire [12:0] h_sync  = scandouble ? H_SYNC  >> 1 : H_SYNC;
wire [12:0] h_vis0  = scandouble ? H_VIS0  >> 1 : H_VIS0;
wire [12:0] h_visn  = scandouble ? H_VISN  >> 1 : H_VISN;
wire  [9:0] v_total = scandouble ? V_TOTAL << 1 : V_TOTAL;
wire  [9:0] v_act   = scandouble ? V_ACT   << 1 : V_ACT;
wire  [9:0] v_sync0 = scandouble ? V_SYNC0 << 1 : V_SYNC0;
wire  [9:0] v_syncn = scandouble ? V_SYNCN << 1 : V_SYNCN;

// ----- per-frame geometry, latched at the top of the vertical blank -----
reg  [5:0] div;          // cycles per pixel
reg [12:0] x0;           // cycle within the line where pixel 0 starts
reg [11:0] w;            // pixels per row shown
reg  [8:0] h;            // rows shown (<= 240)
reg  [7:0] y_off;        // first output row of the picture
reg        y_step;       // show every other source row

// even values only, so the scandoubled half is still an integer
function [5:0] pixel_div(input [11:0] width);
	pixel_div = (width <= 12'd256) ? 6'd20 :
	            (width <= 12'd320) ? 6'd16 :
	            (width <= 12'd352) ? 6'd14 :
	            (width <= 12'd416) ? 6'd12 :
	            (width <= 12'd512) ? 6'd10 :
	            (width <= 12'd640) ? 6'd8  :
	            (width <= 12'd832) ? 6'd6  : 6'd4;
endfunction

// three registered stages (ladder, multiply, centre): fb_width only changes
// at vsync and the result is latched a frame later, so the latency is free
reg   [5:0] div_full;
reg  [18:0] act_len;                                    // cycles, progressive
reg  [12:0] x0_calc;
wire [12:0] act_len_s = scandouble ? act_len[13:1] : act_len[12:0];
always @(posedge clk) begin
	div_full <= pixel_div(fb_width);
	act_len  <= fb_width * div_full;
	x0_calc  <= (act_len_s > h_visn) ? h_vis0 : h_vis0 + ((h_visn - act_len_s) >> 1);
end
wire        v_step   = (fb_height > 12'd262);
wire [11:0] h_shown  = v_step ? {1'b0, fb_height[11:1]} : fb_height;

// ----- counters -----
reg [12:0] hc;
reg  [9:0] vc;
reg  [5:0] dc;          // pixel divider phase
reg [11:0] x;           // pixel being shown
reg        act, done;   // inside the active pixels / finished them this line
reg        row_vis;     // this output line carries a picture row

assign ce_pix = (dc == 0);

wire  [9:0] vc_next = (vc == v_total - 1'd1) ? 10'd0 : vc + 1'd1;
wire  [8:0] vn_row  = scandouble ? vc_next[9:1] : vc_next[8:0];   // and of the next line
wire        vn_vis  = (vn_row >= y_off) && (vn_row < y_off + h);

// ----- line buffer: 2 rows x 256 words x 64 bits as four 16-bit banks -----
reg [15:0] bank0[0:511], bank1[0:511], bank2[0:511], bank3[0:511];
reg [15:0] q0, q1, q2, q3;
reg  [8:0] rd_addr;

always @(posedge clk) begin
	if (line_we) begin
		bank0[line_waddr] <= line_wdata[15:0];
		bank1[line_waddr] <= line_wdata[31:16];
		bank2[line_waddr] <= line_wdata[47:32];
		bank3[line_waddr] <= line_wdata[63:48];
	end
	q0 <= bank0[rd_addr];
	q1 <= bank1[rd_addr];
	q2 <= bank2[rd_addr];
	q3 <= bank3[rd_addr];
end

// x is the pixel whose period began at the last ce. At a ce the pixel that
// starts is x+1 (or 0 when the active area begins), and the bank outputs
// must already hold its word: rd_addr is therefore always the word of the
// pixel after the one starting now, which is word 0 while the line is idle.
wire        starting = !act && !done && row_vis && (hc >= x0);
wire [11:0] x_start  = act ? x + 1'd1 : 12'd0;
wire [11:0] x_after  = (act || starting) ? x_start + 1'd1 : 12'd0;
wire [15:0] pix = (x_start[1:0] == 2'd0) ? q0 : (x_start[1:0] == 2'd1) ? q1 : (x_start[1:0] == 2'd2) ? q2 : q3;

always @(posedge clk) begin
	if (reset) begin
		hc <= 0; vc <= 0; dc <= 0;
		x <= 0; act <= 0; done <= 0; row_vis <= 0;
		hs <= 0; vs <= 0; hblank <= 1; vblank <= 1;
		line_req <= 0; line_y <= 0; line_buf <= 0;
		div <= 6'd12; x0 <= 13'd1129; w <= 12'd416; h <= 9'd240; y_off <= 0; y_step <= 0;
		rd_addr <= 0;
		r <= 0; g <= 0; b <= 0;
	end
	else begin
		line_req <= 0;

		// ---- horizontal / vertical counters ----
		if (hc == h_total - 1'd1) begin
			hc <= 0;
			dc <= 0;
			vc <= vc_next;
			done <= 0;
			act  <= 0;
			row_vis <= (vn_row >= y_off) && (vn_row < y_off + h);
			// the row for the coming line, into the half that line will show
			if (vn_vis) begin
				line_req <= 1;
				line_y   <= y_step ? {(vn_row - y_off), 1'b0} : {3'b0, vn_row - y_off};
				line_buf <= vc_next[0];
			end
			// geometry for the next frame, taken as the blank begins
			if (vc_next == v_act) begin
				div    <= scandouble ? div_full >> 1 : div_full;
				w      <= fb_width;
				h      <= (h_shown > 12'd240) ? 9'd240 : h_shown[8:0];
				y_off  <= (h_shown >= 12'd240) ? 8'd0 : (12'd240 - h_shown) >> 1;
				y_step <= v_step;
				x0     <= x0_calc;
			end
		end
		else begin
			hc <= hc + 1'd1;
			dc <= (dc == div - 1'd1) ? 6'd0 : dc + 1'd1;
		end

		// ---- syncs and blanks, updated on the pixel clock only ----
		if (ce_pix) begin
			hs <= (hc < h_sync);
			if (hc < h_sync) begin
				vs     <= (vc >= v_sync0) && (vc < v_sync0 + v_syncn);
				vblank <= (vc >= v_act);
			end

			// active pixels: x0 rounded up to the pixel grid
			if (act) begin
				if (x == w - 1'd1) begin      // the last pixel's period is over
					act    <= 0;
					done   <= 1;
					hblank <= 1;
				end
				else x <= x_start;
			end
			else if (starting) begin
				act    <= 1;
				x      <= 0;
				hblank <= 0;
			end

			rd_addr <= {vc[0], x_after[9:2]};

			// colour of the pixel that starts now (masked by hblank/vblank outside)
			{r, g, b} <= {pix[15:11], pix[15:13], pix[10:5], pix[10:9], pix[4:0], pix[4:2]};
		end
	end
end

endmodule
