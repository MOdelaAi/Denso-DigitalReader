fn main() {
    slint_build::compile("ui/app-window.slint").unwrap();
    embed_windows_icon();
}

// On Windows, generate a 256×256 .ico from assets/icon.png and embed it as
// the executable icon. No-op on other platforms.
#[cfg(windows)]
fn embed_windows_icon() {
    use image::imageops::FilterType;

    println!("cargo:rerun-if-changed=assets/icon.png");

    let out_dir = std::env::var("OUT_DIR").unwrap();
    let ico_path = std::path::Path::new(&out_dir).join("icon.ico");

    // ICO entries max out at 256×256, so resize the 512px source down.
    let img = image::open("assets/icon.png").expect("read assets/icon.png");
    let resized = img.resize_exact(256, 256, FilterType::Lanczos3);
    resized
        .save_with_format(&ico_path, image::ImageFormat::Ico)
        .expect("write icon.ico");

    let mut res = winresource::WindowsResource::new();
    res.set_icon(ico_path.to_str().unwrap());
    res.compile().expect("embed windows resource");
}

#[cfg(not(windows))]
fn embed_windows_icon() {}
