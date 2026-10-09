#pragma once

/// scene draw の可視性の根拠を引く口 (spec/feature/frame-tap.md §6)。
///
/// Pictor の CPU カリングは視錐台判定だけなので、 既定 (この口が無いとき) の
/// scene draw は `frustum-only` で、 行に `visibility_lag_frames` を出さない。
/// 遮蔽クエリの結果を GPU を待たずに読めるホスト (前フレームの結果を持つ等) は
/// これを実装して FrameTap::set_visibility_source() で渡す。

#include "pictor/core/types.h"
#include "pictor/tap/frame_tap_vocabulary.h"

#include <cstdint>

namespace pictor {

class IFrameTapVisibilitySource {
public:
    virtual ~IFrameTapVisibilitySource() = default;

    /// オブジェクトの可視性の根拠。 結果を持たなければ FRUSTUM_ONLY か UNKNOWN。
    virtual FrameTapVisibility visibility(ObjectId object) const = 0;

    /// 遮蔽の根拠が何フレーム前の結果か (厳密なら 0)。
    virtual uint32_t lag_frames() const = 0;
};

} // namespace pictor
