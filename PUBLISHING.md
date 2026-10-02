# Publishing this project on GitHub

This folder is prepared as a **source repository**. The tested executable, matching runtime DLLs, downloader and notices are in the separate `VIMS_v0.4.12.12_Windows_x64.zip` file supplied alongside this source archive. GitHub automatically creates source archives for releases; those archives do not contain the executable.

1. Extract this source archive to a folder, open PowerShell in that folder, and verify `src/App.cpp`, `README.md`, `LICENSE`, `BUILDING.md`, and `.gitignore` are present.
2. Set your Git author name and email if they are not already configured; run `git init -b main`, `git add .`, and `git commit -m "Release v0.4.12.12 source"`.
3. Sign in to GitHub CLI with `gh auth login`. To create a **public** repository and push the code, run:

   ```powershell
   gh repo create VulkanImageMaskStudio --public --source=. --remote=origin --push
   ```

   For an existing empty GitHub repository, use its real URL instead:

   ```powershell
   git remote add origin https://github.com/khant735/VIMS/VulkanImageMaskStudio.git
   git push -u origin main
   ```

   Do not create a second README or license on GitHub when creating an empty repository; both already exist in the source archive.

4. Check the third-party DLL notices and model terms described in `THIRD_PARTY.md` before making a **public** binary release. In the repository page, select **Releases → Draft a new release**. Create tag `v0.4.12.12`, title `Vulkan Image Mask Studio v0.4.12.12`, paste `RELEASE_NOTES.md`, check **Set as a pre-release**, attach the `VIMS_v0.4.12.12_Windows_x64.zip` file in the binary assets box, then publish.
5. Confirm the repository shows `src/` and the new release shows the attached ZIP asset. Download that asset once and verify its SHA-256 against `VIMS_v0.4.12.12_Windows_x64.sha256` supplied with this handoff. The automatic “Source code (zip)” entry is not the runnable app.

A GitHub account and authentication are needed to push or publish. This prepared handoff has not been pushed to any account.
