fn main() {
    // Ensure embedded (bitmap) PT Sans is generated deterministically for the MCU renderer.
    std::env::set_var("SLINT_FONT_SIZES", "10,12,14,16,18,20,28,46");
    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer)
        .with_scale_factor(1.0);
    slint_build::compile_with_config("ui/app.slint", config).unwrap();
}
