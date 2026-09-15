//============================================================================
//  BennuGD control block: the link between the HPS frontend and the FPGA.
//
//  A 64-byte block in DDR3 (CTL_ADDR) that both sides can reach: the HPS
//  through /dev/mem, this module through the framework's DDRAM port.
//  Once per vertical blank it
//    1. reads what the HPS asked for: which of the two framebuffers to
//       show and their geometry,
//    2. writes back a vblank counter (the HPS waits on it: vsync and page
//       flip without hps_io or SPI on the Linux side) and the four
//       joystick words Main_MiSTer feeds hps_io, so pads mapped in the OSD
//       reach the game even though Main holds the evdev devices grabbed.
//
//  Layout, 32-bit words, byte offsets:
//    0 present   HPS -> FPGA  0 shows FB0_ADDR, 1 shows FB1_ADDR
//    4 width     HPS -> FPGA  0 keeps the current value
//    8 height    HPS -> FPGA  0 keeps the current value
//   12 stride    HPS -> FPGA  bytes, 0 keeps the current value
//   32 vblank    FPGA -> HPS  increments every vertical sync
//   36 joy0      FPGA -> HPS  hps_io joystick words: [3:0] R L D U,
//   40 joy1                    [4..] the buttons named by the CONF_STR J line
//   44 joy2
//   48 joy3
//============================================================================

module bennugd_ctl
#(
	parameter [31:0] CTL_ADDR = 32'h23F00000,
	parameter [31:0] FB0_ADDR = 32'h22000000,
	parameter [31:0] FB1_ADDR = 32'h22100000
)
(
	input             clk,          // DDRAM_CLK domain; vs and joy* are sampled in it
	input             reset,
	input             vs,
	input      [31:0] joy0, joy1, joy2, joy3,

	output reg [31:0] fb_base,
	output reg [11:0] fb_width,
	output reg [11:0] fb_height,
	output reg [13:0] fb_stride,

	output reg        ddr_rd,
	output reg        ddr_we,
	output reg  [7:0] ddr_burst,
	output reg [28:0] ddr_addr,     // 64-bit word address
	output reg [63:0] ddr_din,
	output reg  [7:0] ddr_be,
	input      [63:0] ddr_dout,
	input             ddr_dout_ready,
	input             ddr_busy
);

localparam [28:0] CTL_W = CTL_ADDR[31:3];

reg [31:0] vcount;
reg  [2:0] state;
reg        beat;
reg        vs_d;

always @(posedge clk) begin
	vs_d <= vs;

	if (reset) begin
		state     <= 0;
		ddr_rd    <= 0;
		ddr_we    <= 0;
		ddr_burst <= 1;
		ddr_be    <= 8'hFF;
		ddr_addr  <= CTL_W;
		ddr_din   <= 0;
		fb_base   <= FB0_ADDR;
		fb_width  <= 12'd416;
		fb_height <= 12'd240;
		fb_stride <= 14'd832;
		vcount    <= 0;
		beat      <= 0;
	end
	else begin
		// a request is held until the port stops being busy, then dropped
		if (!ddr_busy) begin
			ddr_rd <= 0;
			ddr_we <= 0;
		end

		case (state)
		// wait for the vertical sync
		0: if (vs & ~vs_d) begin
				vcount <= vcount + 1'd1;
				state  <= 1;
			end

		// read the two 64-bit words the HPS writes (present/width, height/stride)
		1: if (!ddr_busy) begin
				ddr_rd    <= 1;
				ddr_burst <= 2;
				ddr_addr  <= CTL_W;
				beat      <= 0;
				state     <= 2;
			end

		2: if (ddr_dout_ready) begin
				if (!beat) begin
					fb_base <= ddr_dout[0] ? FB1_ADDR : FB0_ADDR;
					if (ddr_dout[43:32] != 0) fb_width <= ddr_dout[43:32];
				end
				else begin
					if (ddr_dout[11:0]  != 0) fb_height <= ddr_dout[11:0];
					if (ddr_dout[45:32] != 0) fb_stride <= ddr_dout[45:32];
					state <= 3;
				end
				beat <= ~beat;
			end

		// write vblank + joy0, joy1 + joy2, joy3
		3: if (!ddr_busy) begin
				ddr_we    <= 1;
				ddr_burst <= 1;
				ddr_addr  <= CTL_W + 29'd4;
				ddr_din   <= {joy0, vcount};
				state     <= 4;
			end

		4: if (!ddr_busy) begin
				ddr_we    <= 1;
				ddr_addr  <= CTL_W + 29'd5;
				ddr_din   <= {joy2, joy1};
				state     <= 5;
			end

		5: if (!ddr_busy) begin
				ddr_we    <= 1;
				ddr_addr  <= CTL_W + 29'd6;
				ddr_din   <= {32'd0, joy3};
				state     <= 0;
			end

		default: state <= 0;
		endcase
	end
end

endmodule
