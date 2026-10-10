// ProResForge 根构建脚本:仅声明插件,版本统一由 gradle/libs.versions.toml 提供。
// 各模块(app)在 plugins {} 中 apply false -> apply。
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.compose) apply false
    alias(libs.plugins.kotlin.serialization) apply false
}
