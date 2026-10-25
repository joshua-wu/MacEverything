#!/usr/bin/env zsh
set -euo pipefail

export DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"

xcodebuild \
  -project MacEverything.xcodeproj \
  -scheme MacEverything \
  -configuration Release \
  -quiet \
  build
