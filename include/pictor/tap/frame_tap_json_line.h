#pragma once

/// 1 フレーム分の描画リストを改行を含まない 1 行の JSON にする
/// (spec/feature/frame-tap.md §3)。 依存ライブラリなし。

#include "pictor/tap/frame_tap_types.h"

#include <string>

namespace pictor {

/// `out` を書き換える (clear してから追記するので、 容量は呼び出し間で再利用される)。
/// passes は scene → ui の順に必ず 1 つずつ。 draw は pass ごとに記録順で並ぶ。
void encode_frame_tap_line(const FrameTapFrame& frame, std::string& out);

} // namespace pictor
