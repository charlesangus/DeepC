use cargo_metadata::MetadataCommand;
use duct::cmd;
use std::{io::Result, path::PathBuf};
fn main() -> Result<()> {
    println!("cargo:rerun-if-changed=NULL");
    build_proto()?;
    Ok(())
}

fn build_proto() -> Result<()> {
    #[cfg(feature = "protobuf-vendored")]
    unsafe {
        std::env::set_var("PROTOC", protobuf_src::protoc());
    }

    let metadata = MetadataCommand::new().exec();
    let (circle_of_confusion_package, bokeh_creator_package) = if let Ok(metadata) = metadata {
        let circle_of_confusion_package = metadata
            .packages
            .iter()
            .find(|p| p.name == "circle-of-confusion")
            .unwrap();
        let bokeh_creator_package = metadata
            .packages
            .iter()
            .find(|p| p.name == "bokeh-creator")
            .unwrap();
        (
            circle_of_confusion_package
                .manifest_path
                .as_std_path()
                .to_path_buf(),
            bokeh_creator_package
                .manifest_path
                .as_std_path()
                .to_path_buf(),
        )
    } else {
        let metadata = cmd!("cargo", "metadata")
            .dir(env!("CARGO_MANIFEST_DIR"))
            .read()?;
        println!("{}", metadata);
        (PathBuf::default(), PathBuf::default())
    };

    let mut config = prost_build::Config::new();
    #[cfg(feature = "serde")]
    config.type_attribute(".", "#[derive(serde::Serialize,serde::Deserialize)]");
    #[cfg(feature = "documented")]
    config.type_attribute(".", "#[derive(documented::DocumentedFields)]");
    // config.field_attribute(".", attribute)
    config.extern_path(".circle_of_confusion", "circle_of_confusion");
    config.extern_path(".bokeh_creator", "bokeh_creator");
    // config.extern_path(".bokeh_creator", "::bokeh-creator")
    let includes = [
        "./proto".to_string(),
        circle_of_confusion_package
            .parent()
            .unwrap()
            .join("proto")
            .to_str()
            .unwrap()
            .to_string(),
        bokeh_creator_package
            .parent()
            .unwrap()
            .join("proto")
            .to_str()
            .unwrap()
            .to_string(),
    ];
    config.compile_protos(&["opendefocus.proto"], &includes)?;
    Ok(())
}
