// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

/// Test that bottle extraction rejects path traversal attempts.
#[test]
fn pour_rejects_path_traversal() {
    let tmp = tempfile::tempdir().unwrap();
    let cellar = tmp.path().join("Cellar");
    std::fs::create_dir_all(&cellar).unwrap();

    // Create a tar.gz with a path traversal entry.
    let tar_data = create_malicious_tarball();

    // pour_bottle should reject this.
    // We can't call the function directly (binary crate), so we
    // replicate the path validation logic.
    let result = validate_tar_paths(&tar_data);
    assert!(result.is_err(), "should reject path traversal");
    assert!(
        result.unwrap_err().contains("path traversal"),
        "error should mention path traversal"
    );
}

#[test]
fn pour_rejects_symlink_path_traversal() {
    let tar_data = create_malicious_symlink_tarball();
    let result = validate_tar_paths(&tar_data);
    assert!(result.is_err(), "should reject symlink path traversal");
    assert!(
        result.unwrap_err().contains("path traversal"),
        "error should mention path traversal"
    );
}

#[test]
fn pour_rejects_absolute_entry_path() {
    let tar_data = create_absolute_path_tarball();
    let result = validate_tar_paths(&tar_data);
    assert!(result.is_err(), "should reject absolute paths");
    assert!(
        result.unwrap_err().contains("absolute"),
        "error should mention absolute"
    );
}

#[test]
fn pour_rejects_absolute_symlink_target() {
    let tar_data = create_absolute_symlink_tarball();
    let result = validate_tar_paths(&tar_data);
    assert!(result.is_err(), "should reject absolute symlink targets");
    assert!(
        result.unwrap_err().contains("absolute"),
        "error should mention absolute"
    );
}

#[test]
fn pour_rejects_hardlinks() {
    let tar_data = create_hardlink_tarball();
    let result = validate_tar_paths(&tar_data);
    assert!(result.is_err(), "should reject hardlinks");
    assert!(
        result.unwrap_err().contains("hardlink"),
        "error should mention hardlink"
    );
}

#[test]
fn pour_accepts_normal_paths() {
    let tar_data = create_normal_tarball();
    let result = validate_tar_paths(&tar_data);
    assert!(result.is_ok(), "should accept normal paths");
}

/// Replicate the path validation logic from bottle::pour_bottle.
fn validate_tar_paths(data: &[u8]) -> Result<(), String> {
    let decoder = flate2::read::GzDecoder::new(data);
    let mut archive = tar::Archive::new(decoder);

    for entry in archive.entries().map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path().map_err(|e| e.to_string())?.into_owned();

        // Reject absolute paths.
        if path.is_absolute() {
            return Err(format!(
                "bottle contains absolute entry path: {}",
                path.display()
            ));
        }

        // Reject parent directory traversal.
        if path
            .components()
            .any(|c| matches!(c, std::path::Component::ParentDir))
        {
            return Err(format!(
                "bottle contains entry with path traversal: {}",
                path.display()
            ));
        }

        // Reject hardlinks.
        if entry.header().entry_type().is_hard_link() {
            return Err(format!(
                "bottle contains hardlink (rejected): {}",
                path.display()
            ));
        }

        // Check symlink targets.
        if entry.header().entry_type().is_symlink()
            && let Some(link_target) = entry.link_name().map_err(|e| e.to_string())?
        {
                if link_target.as_ref().is_absolute() {
                    return Err(format!(
                        "bottle contains absolute symlink target: {} -> {}",
                        path.display(),
                        link_target.display()
                    ));
                }
                if link_target
                    .components()
                    .any(|c| matches!(c, std::path::Component::ParentDir))
                {
                    return Err(format!(
                        "bottle contains symlink with path traversal: {} -> {}",
                        path.display(),
                        link_target.display()
                    ));
                }
        }
    }
    Ok(())
}

fn create_malicious_tarball() -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let encoder = flate2::write::GzEncoder::new(&mut buf, flate2::Compression::fast());
        let mut tar = tar::Builder::new(encoder);

        // Use a raw header to bypass the tar crate's own path validation.
        let data = b"malicious content";
        let mut header = tar::Header::new_gnu();
        header.set_size(data.len() as u64);
        header.set_mode(0o644);
        header.set_entry_type(tar::EntryType::Regular);
        // Set the path by writing directly to the header bytes.
        let path_bytes = b"../../etc/evil";
        header.as_gnu_mut().unwrap().name[..path_bytes.len()]
            .copy_from_slice(path_bytes);
        header.set_cksum();
        tar.append(&header, &data[..]).unwrap();

        tar.into_inner().unwrap().finish().unwrap();
    }
    buf
}

fn create_malicious_symlink_tarball() -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let encoder = flate2::write::GzEncoder::new(&mut buf, flate2::Compression::fast());
        let mut tar = tar::Builder::new(encoder);

        // Create a symlink entry with a traversal target.
        let mut header = tar::Header::new_gnu();
        header.set_size(0);
        header.set_mode(0o777);
        header.set_entry_type(tar::EntryType::Symlink);
        // Set a normal entry path.
        let path_bytes = b"tree/2.3.1/lib/evil";
        header.as_gnu_mut().unwrap().name[..path_bytes.len()]
            .copy_from_slice(path_bytes);
        // Set a traversal symlink target.
        let target_bytes = b"../../../../etc/passwd";
        header.as_gnu_mut().unwrap().linkname[..target_bytes.len()]
            .copy_from_slice(target_bytes);
        header.set_cksum();
        tar.append(&header, &[][..]).unwrap();

        tar.into_inner().unwrap().finish().unwrap();
    }
    buf
}

fn create_absolute_path_tarball() -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let encoder = flate2::write::GzEncoder::new(&mut buf, flate2::Compression::fast());
        let mut tar = tar::Builder::new(encoder);

        let data = b"absolute path content";
        let mut header = tar::Header::new_gnu();
        header.set_size(data.len() as u64);
        header.set_mode(0o644);
        header.set_entry_type(tar::EntryType::Regular);
        let path_bytes = b"/etc/cron.d/evil";
        header.as_gnu_mut().unwrap().name[..path_bytes.len()]
            .copy_from_slice(path_bytes);
        header.set_cksum();
        tar.append(&header, &data[..]).unwrap();

        tar.into_inner().unwrap().finish().unwrap();
    }
    buf
}

fn create_absolute_symlink_tarball() -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let encoder = flate2::write::GzEncoder::new(&mut buf, flate2::Compression::fast());
        let mut tar = tar::Builder::new(encoder);

        let mut header = tar::Header::new_gnu();
        header.set_size(0);
        header.set_mode(0o777);
        header.set_entry_type(tar::EntryType::Symlink);
        let path_bytes = b"tree/2.3.1/lib/evil";
        header.as_gnu_mut().unwrap().name[..path_bytes.len()]
            .copy_from_slice(path_bytes);
        let target_bytes = b"/etc/passwd";
        header.as_gnu_mut().unwrap().linkname[..target_bytes.len()]
            .copy_from_slice(target_bytes);
        header.set_cksum();
        tar.append(&header, &[][..]).unwrap();

        tar.into_inner().unwrap().finish().unwrap();
    }
    buf
}

fn create_hardlink_tarball() -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let encoder = flate2::write::GzEncoder::new(&mut buf, flate2::Compression::fast());
        let mut tar = tar::Builder::new(encoder);

        let mut header = tar::Header::new_gnu();
        header.set_size(0);
        header.set_mode(0o644);
        header.set_entry_type(tar::EntryType::Link);
        let path_bytes = b"tree/2.3.1/bin/evil";
        header.as_gnu_mut().unwrap().name[..path_bytes.len()]
            .copy_from_slice(path_bytes);
        let target_bytes = b"/etc/shadow";
        header.as_gnu_mut().unwrap().linkname[..target_bytes.len()]
            .copy_from_slice(target_bytes);
        header.set_cksum();
        tar.append(&header, &[][..]).unwrap();

        tar.into_inner().unwrap().finish().unwrap();
    }
    buf
}

fn create_normal_tarball() -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let encoder = flate2::write::GzEncoder::new(&mut buf, flate2::Compression::fast());
        let mut tar = tar::Builder::new(encoder);

        let data = b"normal content";
        let mut header = tar::Header::new_gnu();
        header.set_size(data.len() as u64);
        header.set_mode(0o644);
        header.set_cksum();
        tar.append_data(&mut header, "tree/2.3.1/bin/tree", &data[..])
            .unwrap();

        tar.into_inner().unwrap().finish().unwrap();
    }
    buf
}
