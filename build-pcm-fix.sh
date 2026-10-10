#!/bin/bash
# 构建 PCM→MP4 修复版本
set -e

echo "=== ProResForge PCM→MP4 修复构建 ==="
echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo

# 同步到 WSL（如果需要）
if [ -d ~/prf ]; then
    echo "同步源码到 ~/prf..."
    rsync -av --exclude='.git' --exclude='build' --exclude='.gradle' \
          /mnt/z/ProResForge/ ~/prf/
    cd ~/prf
else
    cd /mnt/z/ProResForge
fi

echo "运行 Gradle 构建..."
./gradlew :app:assembleDebug 2>&1 | tee build-pcm-fix.log

if [ $? -eq 0 ]; then
    APK_PATH=$(find app/build/outputs/apk/debug -name "*.apk" | head -1)
    if [ -n "$APK_PATH" ]; then
        APK_SIZE=$(stat -f%z "$APK_PATH" 2>/dev/null || stat -c%s "$APK_PATH")
        APK_SHA=$(sha256sum "$APK_PATH" | awk '{print $1}')
        echo
        echo "✓ 构建成功"
        echo "  APK: $APK_PATH"
        echo "  大小: $(numfmt --to=iec-i --suffix=B $APK_SIZE 2>/dev/null || echo $APK_SIZE bytes)"
        echo "  SHA256: $APK_SHA"
        
        # 复制到 dist
        cp "$APK_PATH" /mnt/z/ProResForge/dist/ProResForge-pcm-fix-$(date +%Y%m%d-%H%M).apk
        echo "  已复制到 dist/"
    fi
else
    echo "✗ 构建失败，查看 build-pcm-fix.log"
    exit 1
fi
