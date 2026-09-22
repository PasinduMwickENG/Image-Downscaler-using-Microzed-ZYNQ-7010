`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 09/16/2026 11:18:55 AM
// Design Name: 
// Module Name: image_downscale
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////

module image_downscale #(
    parameter integer C_S_AXI_ADDR_WIDTH = 4,   // 2 registers -> 4 bytes of addr space
    parameter integer C_S_AXI_DATA_WIDTH = 32
) (
    input  wire        aclk,
    input  wire        aresetn,

    // AXI-Stream Slave (from DMA MM2S)
    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,
    input  wire [3:0]  s_axis_tkeep,

    // AXI-Stream Master (to DMA S2MM)
    output reg  [31:0] m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tlast,
    output reg  [3:0]  m_axis_tkeep,

    // ---------------- AXI4-Lite Slave (runtime config) ----------------
    // Register map (word aligned):
    //   0x0 : WIDTH  (input image width,  bits [15:0])
    //   0x4 : HEIGHT (input image height, bits [15:0])
    // Software must write both registers *before* starting a transfer;
    // values are latched at the start of each frame (x_cnt==0, y_cnt==0)
    // and used for that frame's row-wrap / TLAST generation.
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]   s_axi_awaddr,
    input  wire [2:0]                      s_axi_awprot,
    input  wire                            s_axi_awvalid,
    output reg                             s_axi_awready,
    input  wire [C_S_AXI_DATA_WIDTH-1:0]   s_axi_wdata,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  wire                            s_axi_wvalid,
    output reg                             s_axi_wready,
    output reg  [1:0]                      s_axi_bresp,
    output reg                             s_axi_bvalid,
    input  wire                            s_axi_bready,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]   s_axi_araddr,
    input  wire [2:0]                      s_axi_arprot,
    input  wire                            s_axi_arvalid,
    output reg                             s_axi_arready,
    output reg  [C_S_AXI_DATA_WIDTH-1:0]   s_axi_rdata,
    output reg  [1:0]                      s_axi_rresp,
    output reg                             s_axi_rvalid,
    input  wire                            s_axi_rready
);

    // Default size used until software writes the registers.
    localparam [31:0] DEFAULT_WIDTH  = 32'd128;
    localparam [31:0] DEFAULT_HEIGHT = 32'd128;

    // -------------------- AXI4-Lite register bank --------------------
    reg [C_S_AXI_DATA_WIDTH-1:0] slv_reg0 = DEFAULT_WIDTH;   // WIDTH
    reg [C_S_AXI_DATA_WIDTH-1:0] slv_reg1 = DEFAULT_HEIGHT;  // HEIGHT
    reg                          aw_en;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= 2'b0;
            aw_en         <= 1'b1;
            slv_reg0      <= DEFAULT_WIDTH;
            slv_reg1      <= DEFAULT_HEIGHT;
        end else begin
            // Write address handshake
            if (~s_axi_awready && s_axi_awvalid && s_axi_wvalid && aw_en) begin
                s_axi_awready <= 1'b1;
                aw_en         <= 1'b0;
            end else if (s_axi_bvalid && s_axi_bready) begin
                aw_en         <= 1'b1;
                s_axi_awready <= 1'b0;
            end else begin
                s_axi_awready <= 1'b0;
            end

            // Write data handshake + register update
            if (~s_axi_wready && s_axi_wvalid && s_axi_awvalid && aw_en) begin
                s_axi_wready <= 1'b1;
                case (s_axi_awaddr[C_S_AXI_ADDR_WIDTH-1:2])
                    2'h0: slv_reg0 <= s_axi_wdata;
                    2'h1: slv_reg1 <= s_axi_wdata;
                    default: ;
                endcase
            end else begin
                s_axi_wready <= 1'b0;
            end

            // Write response
            if (s_axi_awready && s_axi_awvalid && ~s_axi_bvalid && s_axi_wready && s_axi_wvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b0;
            end else if (s_axi_bready && s_axi_bvalid) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

    // Read channel
    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rresp   <= 2'b0;
        end else begin
            if (~s_axi_arready && s_axi_arvalid) begin
                s_axi_arready <= 1'b1;
            end else begin
                s_axi_arready <= 1'b0;
            end

            if (s_axi_arready && s_axi_arvalid && ~s_axi_rvalid) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b0;
                case (s_axi_araddr[C_S_AXI_ADDR_WIDTH-1:2])
                    2'h0: s_axi_rdata <= slv_reg0;
                    2'h1: s_axi_rdata <= slv_reg1;
                    default: s_axi_rdata <= 32'b0;
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

    // -------------------- Downscale datapath --------------------
    // 32-bit counters so the full width/height range the software can write
    // is supported (no 16-bit clamp).
    reg [31:0] x_cnt = 0;
    reg [31:0] y_cnt = 0;
    reg [31:0] cur_width  = DEFAULT_WIDTH;
    reg [31:0] cur_height = DEFAULT_HEIGHT;

    // m_axis_tdata/tvalid/tlast form a single-stage "skid buffer": once a
    // kept pixel is latched into it, it holds tvalid high until the
    // downstream actually accepts it (m_axis_tready). We only ever accept a
    // new input beat (s_axis_tready) while that buffer is empty (or being
    // drained this very cycle), so a slow/stalling downstream can never
    // cause an input pixel to be silently dropped or overwritten -
    // regardless of image resolution or how long the stall lasts.
    wire out_busy = m_axis_tvalid && !m_axis_tready;
    assign s_axis_tready = aresetn && !out_busy;

    always @(posedge aclk) begin
        if (!aresetn) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tkeep  <= 4'hF;
            x_cnt         <= 0;
            y_cnt         <= 0;
            cur_width     <= DEFAULT_WIDTH;
            cur_height    <= DEFAULT_HEIGHT;
        end else begin
            // Drain the output register once the downstream accepts it.
            if (m_axis_tvalid && m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
            end

            // Latch the configured size at the start of every frame so a
            // mid-stream register write can't tear the current frame.
            if (x_cnt == 0 && y_cnt == 0) begin
                cur_width  <= slv_reg0;
                cur_height <= slv_reg1;
            end

            if (s_axis_tvalid && s_axis_tready) begin
                // Keep only top-left pixel of every 2x2 block
                if ((x_cnt[0] == 1'b0) && (y_cnt[0] == 1'b0)) begin
                    m_axis_tdata  <= s_axis_tdata;
                    m_axis_tvalid <= 1'b1;
                    m_axis_tkeep  <= 4'hF;

                    // Generate TLAST on the last output pixel
                    if ((x_cnt == cur_width-2) && (y_cnt == cur_height-2))
                        m_axis_tlast <= 1'b1;
                end

                // Advance counters on every accepted input beat.
                if (x_cnt == cur_width-1) begin
                    x_cnt <= 0;
                    // Wrap y_cnt modulo cur_height so the counters
                    // free-run correctly across frame boundaries
                    // (without this, y_cnt ends a frame at cur_height
                    // instead of 0, and never lines up with
                    // cur_height-2 again, so TLAST is never generated
                    // on the next frame).
                    y_cnt <= (y_cnt == cur_height-1) ? 0 : y_cnt + 1;
                end else begin
                    x_cnt <= x_cnt + 1;
                end
            end
        end
    end

endmodule
