fn main() {
    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .include("../../../include")
        .include("../../../src")
        .files(&[
            "../../../src/subband.cpp",
            "../../../src/mdct.cpp",
            "../../../src/quantize.cpp",
            "../../../src/huffman.cpp",
            "../../../src/reservoir.cpp",
            "../../../src/bitstream.cpp",
            "../../../src/encoder.cpp",
        ])
        .opt_level(2)
        .compile("glint");
}
