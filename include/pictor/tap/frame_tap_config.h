#pragma once

/// フレームタップの有効化書式 (`PICTOR_FRAME_TAP` / `RendererConfig::frame_tap`) の
/// 解釈と、 そこからの配送先の生成 (spec/feature/frame-tap.md §2)。

#include "pictor/tap/frame_tap_sink.h"

#include <memory>
#include <string>
#include <string_view>

namespace pictor {

/// 環境変数名。
inline constexpr const char* kFrameTapEnvVar = "PICTOR_FRAME_TAP";

enum class FrameTapOutput : uint8_t {
    NONE   = 0,  ///< 無効
    STDOUT = 1,  ///< 標準出力
    FILE   = 2,  ///< ファイル追記
};

struct FrameTapSetting {
    FrameTapOutput output = FrameTapOutput::NONE;
    std::string    path;   ///< output == FILE のときだけ意味を持つ
};

/// 未設定 / 空 / "0" / "off" (大文字小文字無視) → NONE、 "stdout" / "-" → STDOUT、
/// それ以外 → FILE (値をそのままパスとして使う)。
FrameTapSetting parse_frame_tap_setting(std::string_view value);

/// `kFrameTapEnvVar` の値 (未設定なら空)。
std::string read_frame_tap_env();

/// 設定から配送先を作る。 NONE は null を返す (エラーではない)。
/// FILE を開けないときは null と理由を `error` に入れる。 別の配送先へは逃げない。
std::unique_ptr<IFrameTapSink> make_frame_tap_sink(const FrameTapSetting& setting,
                                                   std::string*           error = nullptr);

} // namespace pictor
