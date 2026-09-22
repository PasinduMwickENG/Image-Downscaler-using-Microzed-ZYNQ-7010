`timescale 1ns / 1ps

module tb_image_downscale;
    reg aclk = 0;
    reg aresetn = 0;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;
    reg  [3:0]  s_axis_tkeep;

    wire [31:0] m_axis_tdata;
    wire        m_axis_tvalid;
    reg         m_axis_tready;
    wire        m_axis_tlast;
    wire [3:0]  m_axis_tkeep;

    reg  [3:0]  s_axi_awaddr;
    reg         s_axi_awvalid;
    wire        s_axi_awready;
    reg  [31:0] s_axi_wdata;
    reg  [3:0]  s_axi_wstrb;
    reg         s_axi_wvalid;
    wire        s_axi_wready;
    wire [1:0]  s_axi_bresp;
    wire        s_axi_bvalid;
    reg         s_axi_bready;
    reg  [3:0]  s_axi_araddr;
    reg         s_axi_arvalid;
    wire        s_axi_arready;
    wire [31:0] s_axi_rdata;
    wire [1:0]  s_axi_rresp;
    wire        s_axi_rvalid;
    reg         s_axi_rready;

    integer errors = 0;

    image_downscale dut (
        .aclk(aclk), .aresetn(aresetn),
        .s_axis_tdata(s_axis_tdata), .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready), .s_axis_tlast(s_axis_tlast), .s_axis_tkeep(s_axis_tkeep),
        .m_axis_tdata(m_axis_tdata), .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready), .m_axis_tlast(m_axis_tlast), .m_axis_tkeep(m_axis_tkeep),
        .s_axi_awaddr(s_axi_awaddr), .s_axi_awprot(3'b0), .s_axi_awvalid(s_axi_awvalid), .s_axi_awready(s_axi_awready),
        .s_axi_wdata(s_axi_wdata), .s_axi_wstrb(s_axi_wstrb), .s_axi_wvalid(s_axi_wvalid), .s_axi_wready(s_axi_wready),
        .s_axi_bresp(s_axi_bresp), .s_axi_bvalid(s_axi_bvalid), .s_axi_bready(s_axi_bready),
        .s_axi_araddr(s_axi_araddr), .s_axi_arprot(3'b0), .s_axi_arvalid(s_axi_arvalid), .s_axi_arready(s_axi_arready),
        .s_axi_rdata(s_axi_rdata), .s_axi_rresp(s_axi_rresp), .s_axi_rvalid(s_axi_rvalid), .s_axi_rready(s_axi_rready)
    );

    always #5 aclk = ~aclk;

    task axi_write(input [3:0] addr, input [31:0] data);
        begin
            @(posedge aclk);
            s_axi_awaddr  <= addr;
            s_axi_awvalid <= 1'b1;
            s_axi_wdata   <= data;
            s_axi_wstrb   <= 4'hF;
            s_axi_wvalid  <= 1'b1;
            s_axi_bready  <= 1'b1;
            @(posedge aclk);
            while (!(s_axi_awready && s_axi_wready)) @(posedge aclk);
            s_axi_awvalid <= 1'b0;
            s_axi_wvalid  <= 1'b0;
            while (!s_axi_bvalid) @(posedge aclk);
            @(posedge aclk);
            s_axi_bready <= 1'b0;
        end
    endtask

    integer word_i;
    integer out_count;
    integer expected_out;
    reg [31:0] expected_data;
    reg bp_enable = 0;

    // Free-running backpressure generator, enabled/disabled per frame.
    always @(posedge aclk) begin
        if (bp_enable)
            m_axis_tready <= ($random % 3) != 0; // ~33% stall
        else
            m_axis_tready <= 1'b1;
    end

    task run_frame(input [15:0] width, input [15:0] height, input backpressure);
        begin
            axi_write(4'h0, {16'b0, width});
            axi_write(4'h4, {16'b0, height});

            out_count   = 0;
            expected_out = (width/2) * (height/2);
            bp_enable    = backpressure;

            fork
                // Driver: send width*height pixels, pixel value = its raster index
                begin
                    for (word_i = 0; word_i < width*height; word_i = word_i + 1) begin
                        s_axis_tdata  <= word_i;
                        s_axis_tvalid <= 1'b1;
                        s_axis_tlast  <= (word_i == width*height-1);
                        @(posedge aclk);
                        while (!s_axis_tready) @(posedge aclk);
                    end
                    s_axis_tvalid <= 1'b0;
                    s_axis_tlast  <= 1'b0;
                end
                // Checker: verify every output beat is the top-left pixel of its 2x2 block
                begin
                    while (out_count < expected_out) begin
                        @(posedge aclk);
                        if (m_axis_tvalid && m_axis_tready) begin
                            // Expected raster index of top-left pixel of block out_count
                            expected_data = ((out_count / (width/2)) * 2) * width + (out_count % (width/2)) * 2;
                            if (m_axis_tdata !== expected_data) begin
                                $display("MISMATCH at out_count=%0d: got %0d expected %0d (w=%0d h=%0d)",
                                          out_count, m_axis_tdata, expected_data, width, height);
                                errors = errors + 1;
                            end
                            if (out_count == expected_out-1 && !m_axis_tlast) begin
                                $display("ERROR: TLAST not asserted on final beat (w=%0d h=%0d)", width, height);
                                errors = errors + 1;
                            end
                            if (out_count != expected_out-1 && m_axis_tlast) begin
                                $display("ERROR: TLAST asserted early at out_count=%0d (w=%0d h=%0d)", out_count, width, height);
                                errors = errors + 1;
                            end
                            out_count = out_count + 1;
                        end
                    end
                end
            join
            bp_enable = 0;
            @(posedge aclk);
            $display("Frame %0dx%0d done: %0d/%0d output beats OK", width, height, out_count, expected_out);
        end
    endtask

    initial begin
        s_axis_tvalid = 0; s_axis_tdata = 0; s_axis_tlast = 0; s_axis_tkeep = 4'hF;
        m_axis_tready = 1;
        s_axi_awaddr = 0; s_axi_awvalid = 0; s_axi_wdata = 0; s_axi_wstrb = 0; s_axi_wvalid = 0; s_axi_bready = 0;
        s_axi_araddr = 0; s_axi_arvalid = 0; s_axi_rready = 0;

        #20 aresetn = 1;
        @(posedge aclk);

        // Frame 1: 8x4, no backpressure
        run_frame(16'd8, 16'd4, 0);
        // Frame 2: different size back-to-back (tests frame-boundary reset fix)
        run_frame(16'd6, 16'd6, 0);
        // Frame 3: same size run twice in a row (regression check for the original bug)
        run_frame(16'd8, 16'd4, 0);
        run_frame(16'd8, 16'd4, 0);
        // Frame 4: odd non-power-of-two size with random back-pressure
        run_frame(16'd10, 16'd6, 1);
        run_frame(16'd10, 16'd6, 1);

        if (errors == 0)
            $display("ALL TESTS PASSED");
        else
            $display("%0d ERRORS", errors);

        $finish;
    end

    initial begin
        #100000;
        $display("TIMEOUT");
        $finish;
    end
endmodule
