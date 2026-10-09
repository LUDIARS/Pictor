#pragma once

/// 1 フレーム分の描画リストを改行を含まない 1 行の JSON にする
/// (spec/feature/frame-tap.md §3)。 依存ライブラリなし。

#include "pictor/tap/frame_tap_types.h"

#include <string>

namespace pictor {

/// frame 行。 `out` を書き換える (clear してから追記するので、 容量は呼び出し間で
/// 再利用される)。 passes は開いた順、 draw は pass ごとに記録順で並ぶ。
/// 数値は有限であること (FrameTap が有限でない draw を dropped に回してから呼ぶ)。
void encode_frame_tap_line(const FrameTapFrame& frame, std::string& out);

/// end 行 `{"contract":"render-tap/1","seq":N,"end":"shutdown|error"}`。
void encode_frame_tap_end_line(uint64_t seq, FrameTapEndReason reason, std::string& out);

} // namespace pictor
