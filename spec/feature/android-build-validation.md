# Androidビルド確認と実機確認の前提

記録日: 2026-09-10。

## 機能別プラットフォーム対応表への追補

`platform-feature-matrix.md` のAndroidビルド失敗記録に対する追加検証。元表の47967d8とは異なり、本検証はローカルmain 5822f253に本変更を適用した構成である。元snapshotの結果を消さず、構成別の結果として扱う。

| 項目 | 今回の判定 |
|---|---|
| Android ARM64 / API 26 / NDK 21.4.7075529 / Debug | CMake configureとpictor静的ライブラリ生成に成功 |
| PC-FEAT-DOD-02 フレーム・プールメモリ確保 | Android向けコンパイル成功。64-byte境界、確保失敗時のbad_alloc、freeとの組み合わせを維持 |
| Visus JSON数値変換 | Android向けコンパイル成功。旧NDK向けにclassic localeのstream変換を使用。実行検証は未実施 |
| Vulkan ROV拡張 | ヘッダに拡張定義がない構成では有効化しない。既存のinterlock/atomic選択と診断ログを維持 |
| Vulkan API要求 | 既存の1.2要求を維持。古いヘッダでも表現できるVK_MAKE_VERSIONを使用 |
| Androidアプリへの最終リンク・実機描画・背景復帰・surface/device復旧 | 未実施 |

静的ライブラリ生成はAPKへの最終リンク、端末GPUの対応、全機能の動作確認を意味しない。別のnative実装snapshotや未選択の機能へ結果を転用しない。

## ビルド条件

- CMake 4.1.2、Ninja、NDK 21.4.7075529。
- ANDROID_ABI=arm64-v8a、ANDROID_PLATFORM=android-26、CMAKE_BUILD_TYPE=Debug。
- PICTOR_BUILD_DEMO=OFF、PICTOR_BUILD_TESTS=OFF、PICTOR_BUILD_TOOLS=OFF。
- PICTOR_ENABLE_RIVE=OFF、PICTOR_ENABLE_HW_COUNTERS=OFF、PICTOR_USE_LARGE_PAGES=OFF（既定値）。
- `cmake --build build/android-arm64 --target pictor --parallel 4` がexit 0、libpictor.aを生成。
- 既存の未使用引数やVulkan構造体初期化の警告あり。

## 修正理由

NDK r21のstdlib.hではaligned_allocがAPI 28、posix_memalignがAPI 17から提供される。AndroidのPoolAllocatorとFrameAllocatorでposix_memalignを使い、エラーコードを確認する。その他OSの確保経路は従来どおり。

次に判明した旧libc++の浮動小数点charconv未提供へ、Android限定のlocale非依存変換を追加。有限性、末尾までの読み取り、非ゼロ値のゼロへのunderflowを確認する。JSONの既存文法検証は維持する。

NDK同梱VulkanヘッダにはROV拡張がないため、対応構造体の使用・照会・有効化をヘッダの拡張定義で条件化する。型定義を独自に捏造しない。

## Android実機確認の前提

調査したPictor本体とnative実装worktreeにはAndroidManifest.xml、Gradleプロジェクト、APKが見当たらない。docs/android-build.mdでもJNI/Gradleホストは未実装として記載されている。Excubitorのサービス一覧にPictorのAndroid/ADB起動定義はない。ADBサーバーの既定ポート5037の待受も確認できていない。端末接続自体の有無は未確定。

実機確認には対象端末とUSBデバッグ許可、起動可能なAndroidホストアプリ、Pictor本体を起点とするExcubitor実行経路が必要。条件が揃った後、Concordiaへtesting claimを入れて初回描画、背景復帰、surface再生成を確認し、releaseする。device lossは安全な再現方法がある範囲で確認する。

今回、アプリやADBサーバーは起動せず、実機テストも実施していない。①の静的ライブラリビルドと②の実機確認を別々に判定する。
