#pragma once

#include "pictor/visus/visus.h"

#include <vector>

namespace pictor {

// ============================================================
// VisusRegistry — central VisusDesc 置き場
// ============================================================
// MaterialRegistry と同じ形。 register → handle 取得 → 後で get で
// 引く。 handle は登録順の連番 (0 始まり)。
//
// ResourceRef を解決する責務は持たない (IResourceLoader 側)。
// 既に handle 解決済みの VisusDesc を受け取る前提。

class VisusRegistry {
public:
    VisusRegistry() = default;

    /// VisusDesc を登録。 desc は move される。
    /// 戻り値: 割り振られた VisusHandle。
    VisusHandle register_visus(VisusDesc desc);

    /// handle から VisusDesc を引く。 無ければ nullptr。
    const VisusDesc* get(VisusHandle handle) const;

    /// 次に割り振る handle (peek のみ、 消費しない)
    VisusHandle next_handle() const { return next_handle_; }

    /// 登録済み数
    size_t count() const { return visus_.size(); }

private:
    std::vector<VisusDesc> visus_;
    VisusHandle            next_handle_ = 0;
};

} // namespace pictor
