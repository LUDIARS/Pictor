# Androidデモ集APK

Pictorだけでデモ選択APKをビルドする。Ergoへ依存しない。端末への導入・起動・終了はExcubitorのAndroid機能が担当し、本APKにADBやExcubitorクライアントを含めない。

初期収録はInstanced spheres（SimpleRendererのインスタンス描画）とSurface recovery（色の変化するrender pass）。それぞれ別のDemoPipeline実装を持つ。共通化するのはAndroidライフサイクルとVulkanフレーム提出であり、描画パイプラインを統合しない。既存PCデモの全移植ではない。

## ビルド

Android SDK platform 31、build-tools 34.0.0、NDK、JDK 11以上、CMake 3.20以上、Ninja、Node.jsを用意する。ANDROID_HOME、ANDROID_NDK_HOME、JAVA_HOMEを明示し、必要ならNINJAへ実行ファイルを指定する。

`node demo/android/build.mjs`

出力はbuild/android-demo/pictor-demos.apk。ARM64、minSdk 28、Debug署名の検証用APK。Pictor本体のVulkan 1.2要求を維持する。API 26の静的ライブラリ生成とAPKの対応OSは別の判定である。署名鍵はbuild配下にのみ生成する。

## ライフサイクル

Activityのpauseとsurface破棄でGPU資源を解放し、resumeとsurface生成・サイズ変更で再構築する。ANativeWindow参照はネイティブホストが解放する。初期化・描画失敗は画面に表示し、一覧に戻って再選択できる。device lossからの無停止自動復旧は本APKで検証済みとは扱わない。

基本描画デモのUBOは単一バッファのため、この検証ホストはフレームを直列化する。性能測定用構成ではない。実機の初回描画・背景復帰・surface再生成は端末接続後に別途確認する。

## ビルド確認

SDK platform 31、build-tools 34.0.0、NDK 21.4.7075529、JDK 11、CMake 4.1.2で、共有ライブラリの最終リンク、DEX生成、APK生成、Debug署名検証まで成功。Manifestのpackageはcom.ludiars.pictor.demos、起動Activityは.DemoActivity、ARM64向け。実機動作は未確認。ビルドはAndroid互換修正を適用したローカルmainを基点とし、別ブランチの未採用機能を検証済みにしない。
