#!/bin/sh
# Dev loop: kill lab editor, deploy fresh build, relaunch, show log.
cd "$(dirname "$0")"
taskkill //IM VOCALOID4.exe //F >/dev/null 2>&1
python -c "import time;time.sleep(1.5)"
cp dist/version.dll lab/V4/ && mkdir -p lab/V4/vhmods && cp -r dist/vhmods/* lab/V4/vhmods/ 2>/dev/null
(cd lab/V4 && start "" ./VOCALOID4.exe >/dev/null 2>&1 </dev/null &)
python -c "import time;time.sleep(${1:-14})"
cat lab/V4/vhmods/vocahansments.log
