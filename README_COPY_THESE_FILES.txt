NxFrame patch-only files
========================

Copy these files into your existing /home/michalis/nxframe project root, preserving folders.
Do NOT copy a build/ folder. Do NOT delete your DeckLinkAPI folder.

Recommended copy command after extracting this zip:

  cd /home/michalis/nxframe
  cp -av /path/to/extracted_patch/* .
  rm -rf build
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNXFRAME_BUILD_TESTS=OFF
  cmake --build build -j$(nproc)

This patch keeps the local DeckLinkAPI/ folder as the default DeckLink SDK path.
