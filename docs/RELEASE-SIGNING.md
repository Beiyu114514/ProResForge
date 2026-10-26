# ProResForge 发布签名指南

> 签名接线见 `app/build.gradle.kts` 的 `signingConfigs["release"]`;
> 本文是可独立执行的操作手册(新构建机从零配好签名只需本文)。

## 签名密钥管理

### 现状(v1.0.0 已配置)

- Keystore 文件:`proresforge-release.keystore`,alias **`proresforge`**,
  RSA 2048,validity 10000 天。
- 证书 SHA-256 指纹:
  `7A:40:D4:FC:75:F4:BD:4E:B4:DE:A6:45:58:4A:DF:69:43:73:76:0F:0E:59:57:CF:59:A1:1B:08:74:6B:F6:1D`
- 保管位置(**全部在仓库外**):
  1. WSL `~/keys/proresforge-release.keystore`(构建机使用的一份);
  2. WSL `~/keys/backup/`;
  3. Windows `C:\Users\Administrator\ProResForge-signing\`。
  口令记录在与 keystore 同目录的 `proresforge-signing-info.txt`
  (不入仓库、不进任何构建脚本)。
- 构建凭据:项目根目录 `keystore.properties`(**gitignored**,仅存在于构建机):
  ```
  storeFile=/home/ubuntu/keys/proresforge-release.keystore
  storePassword=…
  keyAlias=proresforge
  keyPassword=…
  ```
  文件缺失时构建照常,但产出**未签名** APK(供存档/CI)。

### 密钥生成(仅首次/换新密钥时)

```bash
# 1. 生成 release keystore(注意 alias 用 proresforge,与上文一致)
keytool -genkey -v \
  -keystore ~/keys/proresforge-release.keystore \
  -alias proresforge \
  -keyalg RSA -keysize 2048 -validity 10000 \
  -storepass "YOUR_STORE_PASSWORD" \
  -keypass "YOUR_KEY_PASSWORD" \
  -dname "CN=ProResForge, O=ProResForge contributors"

# 2. 验证密钥(核对指纹是否与上文一致)
keytool -list -v -keystore ~/keys/proresforge-release.keystore
```

### 配置签名(新构建机)

```bash
# 3. 在项目根目录创建 keystore.properties(不入库!)
cd /path/to/ProResForge
cat > keystore.properties <<'EOF'
storeFile=/home/ubuntu/keys/proresforge-release.keystore
storePassword=YOUR_STORE_PASSWORD
keyAlias=proresforge
keyPassword=YOUR_KEY_PASSWORD
EOF

# 4. 确认已忽略(应输出文件名;storeFile 也可用相对路径,相对项目根解析)
git check-ignore keystore.properties
```

换新构建机只需:恢复 keystore + 新建 `keystore.properties`,无需改任何入库文件。

### 密钥备份

**⚠️ 关键**:密钥一旦丢失,无法为已发布的 App 发布更新(同签名校验失败,
用户只能卸载重装)。

```bash
# 至少两个独立存储(现状已有 WSL backup 目录 + Windows 目录,还需离线归档):
# 1. 加密 U 盘
# 2. 密码管理器(1Password/Bitwarden)
# 3. 企业密钥管理系统

# 示例:备份到加密压缩包
tar czf proresforge-keystore-backup-$(date +%Y%m%d).tar.gz \
  ~/keys/proresforge-release.keystore proresforge-signing-info.txt
gpg -c proresforge-keystore-backup-*.tar.gz  # 加密
```

### 构建签名 APK

```bash
# 确认 keystore.properties 配置正确
./gradlew assembleRelease

# 输出位置
ls -lh app/build/outputs/apk/release/app-release.apk

# 验证签名(核对指纹与上文一致)
apksigner verify --verbose app/build/outputs/apk/release/app-release.apk
```

### CI/CD 配置(可选)

如果使用 GitHub Actions / GitLab CI,把 keystore 与口令放进 secrets:

```yaml
# .github/workflows/release.yml
- name: Decode keystore
  run: |
    echo "${{ secrets.KEYSTORE_BASE64 }}" | base64 -d > release.keystore

- name: Create keystore.properties
  run: |
    cat > keystore.properties <<EOF
    storeFile=release.keystore
    storePassword=${{ secrets.STORE_PASSWORD }}
    keyAlias=proresforge
    keyPassword=${{ secrets.KEY_PASSWORD }}
    EOF
```

## 发布纪律

- **versionCode 递增**:每次对外分发(含同一版本的重发修复)**必须递增
  `versionCode`**(`app/build.gradle.kts`),versionName 才按语义变更。
- **渠道**:GitHub Releases 直发自签 APK(不走 Google Play;上架 Play 时改走
  `:app:bundleRelease` + Play App Signing,versionCode 重新起算)。
- **发布条目固定两件产物,缺一不可**(GPL 合规):
  1. `ProResForge-<版本>.apk`(release 签名);
  2. `ProResForge-source-v<版本>.zip`(对应 git tag 的源码快照)。

## R8 / minify

决策与前提记录在 `app/build.gradle.kts` release 块注释(C3):默认
`isMinifyEnabled = false`;将来开启属独立变更,前提是 keep 规则齐全
(`app/proguard-rules.pro`)且重跑全套设备冒烟——**没有冒烟不开 R8**。

## 常见问题

### Q1: 首次构建提示 "keystore.properties not found"?
A: 正常,按上述步骤创建即可。文件缺失时构建不失败,产出未签名 APK
   (`app-release-unsigned.apk`)。

### Q2: 如何查看已有 keystore 信息?
```bash
keytool -list -v -keystore ~/keys/proresforge-release.keystore
```

### Q3: 密钥过期怎么办?
A: release keystore 有效期 10000 天(约 27 年),到期前需生成新密钥并发布过渡版本。

### Q4: 忘了口令?
A: 查同目录 `proresforge-signing-info.txt`(三处备份各有)。三处都丢了 =
   keystore 不可用,只能换新密钥发「卸载重装」版本——这就是备份三处的意义。
