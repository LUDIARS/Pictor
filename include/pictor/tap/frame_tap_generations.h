#pragma once

/// instance ハンドルの世代 (`generation`、 spec/feature/frame-tap.md §4)。
///
/// 契約: 同じ instance で外見 (mesh + 並べ替えた material[]) が変わるなら generation を
/// +1 する。 Pictor の ObjectId は使い回さないが、 UI の instance は描画リストの添字で
/// フレームごとに別の物を指しうる。 そこで instance ごとに直前の外見を覚え、 外見が
/// 変わったら世代を上げる (受信側の instance-reuse 違反を Pictor から出さない)。
///
/// 表は instance を添字にした平坦な配列 (scene 用と ui 用の 2 本)。 タップ ON の間だけ
/// 伸びる。 添字が kMaxTrackedInstances 以上の instance は追跡できないので false を返す
/// (呼び出し側はその draw を dropped に数える)。

#include <cstdint>
#include <vector>

namespace pictor {

enum class FrameTapInstanceSpace : uint8_t {
    SCENE = 0,  ///< ObjectId
    UI    = 1,  ///< UI 内の instance (出力は kFrameTapUiInstanceBase + 値)
};

class FrameTapGenerations {
public:
    /// 追跡できる instance の上限 (表 1 本あたり。 1 要素 16 バイト)。
    static constexpr uint32_t kMaxTrackedInstances = 1u << 22;

    /// `appearance` (外見のハッシュ) で instance を観測し、 その世代を `generation` に書く。
    /// 初見は 0、 前回と外見が違えば +1。 追跡できなければ false。
    bool observe(FrameTapInstanceSpace space, uint32_t instance, uint64_t appearance,
                 uint32_t& generation);

    /// 新しい出力の流れの始まり。 受信側の状態も新しくなるので全て忘れる。
    void reset();

private:
    struct Entry {
        uint64_t appearance = 0;
        uint32_t generation = 0;
        uint32_t seen       = 0;
    };

    std::vector<Entry> scene_;
    std::vector<Entry> ui_;
};

} // namespace pictor
