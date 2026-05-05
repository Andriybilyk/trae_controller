fn main() {
    // Ensure embedded (bitmap) PT Sans is generated deterministically for the MCU renderer.
    // Bitmap font sizes used by MCU renderer. Keep the list compact to avoid build-time OOM.
    std::env::set_var("SLINT_FONT_SIZES", "10,11,12,13,14,15,16,18,20,22,24,26,28,32,36,38,42,46,64,80,96,120");
    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer)
        .with_scale_factor(1.0);
    slint_build::compile_with_config("ui/app.slint", config).unwrap();
}
