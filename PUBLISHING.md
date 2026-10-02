# Publishing VIMS releases

Repository: https://github.com/khant735/VIMS

The repository contains source code and documentation. Runnable Windows builds are published separately as release assets. GitHub’s automatic source archives do not contain the executable.

## Prepare the Windows package

1. Build the application using BUILDING.md.
2. Put the versioned executable, compatible runtime DLLs, model downloader, taxonomy files, and user documentation in one folder.
3. Include LICENSE, THIRD_PARTY.md, and the license and notice documents supplied with each bundled dependency under notices/.
4. Ensure README.md links work from the extracted package.
5. Exclude downloaded model weights, private training samples, logs, and personal files.
6. Compress the folder as VIMS_v0.4.12.12_Windows_x64.zip.

Compilation and archive integrity checks do not establish that the application works correctly on Windows. Describe completed testing and remaining limitations accurately in RELEASE_NOTES.md.

## Generate the checksum

Open PowerShell in the folder containing the final ZIP and run:

```powershell
$zip = 'VIMS_v0.4.12.12_Windows_x64.zip'
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $zip" | Set-Content -LiteralPath 'VIMS_v0.4.12.12_Windows_x64.sha256' -Encoding ascii
```

Regenerate the checksum whenever the ZIP changes.

## Publish on GitHub

1. Commit the source and documentation changes to the existing khant735/VIMS repository.
2. Open Releases and create a release, or edit the existing release when correcting its packaging.
3. For this version, use tag v0.4.12.12 and title Vulkan Image Mask Studio v0.4.12.12.
4. Paste RELEASE_NOTES.md into the release description.
5. Select Set as a pre-release while this version remains a preview.
6. Attach both:
   - VIMS_v0.4.12.12_Windows_x64.zip
   - VIMS_v0.4.12.12_Windows_x64.sha256
7. When replacing an existing package, replace both assets with the matching new pair.
8. Publish or save the release.

For future versions, update the tag, executable name, asset filenames, documentation, and checksum commands consistently.

## Verify the published release

Download both assets from GitHub into a fresh folder.

Run:

```powershell
Get-FileHash .\VIMS_v0.4.12.12_Windows_x64.zip -Algorithm SHA256
Get-Content .\VIMS_v0.4.12.12_Windows_x64.sha256
```

Confirm that the hashes match. Extract the ZIP and check that the executable, runtime DLLs, downloader, documentation, and component notices are present.

Test launching, model downloading, image analysis, and exports on Windows. Keep any unconfirmed behavior documented in the release notes.
