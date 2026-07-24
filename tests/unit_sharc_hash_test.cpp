// SHaRC 空間ハッシュの CPU 契約テスト (SHARC_EXT_REVIEW Gate 1)。
//
// GLSL (shaders/sharc/sharc_common.glsl) と同一のセマンティクスを
// sharc_types.h の純関数上でシミュレートし、 open addressing の
// 衝突 / 削除 (TOMBSTONE) / 再挿入 / wraparound / probe 上限を検証する。
// 完了条件 (review P0-02):
//   - collision → 先頭 delete → 後続 lookup が成功する
//   - tombstone を跨いだ lookup / insert が成功する
//   - table wraparound でも同じ結果になる
//   - probe 上限到達で无言に消えず INVALID が返る

#include "pictor/gi/sharc_types.h"
#include "test_common.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace pictor;

namespace {

constexpr uint32_t kTombstone = 0xFFFFFFFEu;

struct Grid {
    int32_t x, y, z;
    uint32_t level;
};

/// GLSL sharcFindSlot と同一セマンティクス。
uint32_t find_slot(const std::vector<uint32_t>& keys, Grid g) {
    const uint32_t table = static_cast<uint32_t>(keys.size());
    const uint32_t fp = sharc_fingerprint(g.x, g.y, g.z, g.level);
    const uint32_t home = sharc_slot_hash(g.x, g.y, g.z, g.level, table);
    for (uint32_t i = 0; i < kSharcProbeLimit; ++i) {
        const uint32_t s = (home + i) & (table - 1);
        if (keys[s] == fp) return s;
        if (keys[s] == 0u) return kSharcSlotInvalid;
        // TOMBSTONE は占有扱いで読み飛ばす
    }
    return kSharcSlotInvalid;
}

/// GLSL sharcInsertSlot と同一セマンティクス (シングルスレッド版)。
uint32_t insert_slot(std::vector<uint32_t>& keys, Grid g, bool* inserted) {
    const uint32_t table = static_cast<uint32_t>(keys.size());
    const uint32_t fp = sharc_fingerprint(g.x, g.y, g.z, g.level);
    const uint32_t home = sharc_slot_hash(g.x, g.y, g.z, g.level, table);
    if (inserted) *inserted = false;
    for (uint32_t i = 0; i < kSharcProbeLimit; ++i) {
        const uint32_t s = (home + i) & (table - 1);
        if (keys[s] == fp) return s;
        if (keys[s] == 0u || keys[s] == kTombstone) {
            keys[s] = fp;
            if (inserted) *inserted = true;
            return s;
        }
    }
    return kSharcSlotInvalid;
}

void evict_slot(std::vector<uint32_t>& keys, uint32_t slot) {
    keys[slot] = kTombstone;   // GLSL compact パスと同じ (0 に戻さない)
}

/// home スロットが target になる (grid, level) を総当たりで探す。
Grid find_grid_with_home(uint32_t table, uint32_t target, int variant) {
    int found = 0;
    for (int32_t x = -200; x < 200; ++x) {
        for (int32_t z = -20; z < 20; ++z) {
            Grid g{x, 0, z, 0};
            if (sharc_slot_hash(g.x, g.y, g.z, g.level, table) == target) {
                if (found == variant) return g;
                ++found;
            }
        }
    }
    PT_ASSERT(false, "collision candidate not found (test setup)");
    return Grid{0, 0, 0, 0};
}

} // namespace

int main() {
    constexpr uint32_t kTable = 64;

    // ── 1. 挿入 → 検索の往復 ──
    {
        std::vector<uint32_t> keys(kTable, 0u);
        Grid a{3, 1, -7, 2};
        bool ins = false;
        const uint32_t s = insert_slot(keys, a, &ins);
        PT_ASSERT(ins, "fresh insert must claim a slot");
        PT_ASSERT(find_slot(keys, a) == s, "find after insert");
    }

    // ── 2. 衝突チェイン: A, B 同 home → A 削除 → B が検索可能 (P0-02) ──
    {
        std::vector<uint32_t> keys(kTable, 0u);
        const Grid a = find_grid_with_home(kTable, 17, 0);
        const Grid b = find_grid_with_home(kTable, 17, 1);
        PT_ASSERT(sharc_fingerprint(a.x, a.y, a.z, a.level) !=
                      sharc_fingerprint(b.x, b.y, b.z, b.level),
                  "test setup: distinct fingerprints");
        const uint32_t sa = insert_slot(keys, a, nullptr);
        const uint32_t sb = insert_slot(keys, b, nullptr);
        PT_ASSERT(sb == ((sa + 1) & (kTable - 1)),
                  "B probes past occupied A");
        evict_slot(keys, sa);
        PT_ASSERT(find_slot(keys, b) == sb,
                  "B remains findable after A eviction (tombstone)");
        // 旧実装 (key=0 戻し) ではここで INVALID になっていた
    }

    // ── 3. TOMBSTONE 再利用: 削除跡へ新キーが入る ──
    {
        std::vector<uint32_t> keys(kTable, 0u);
        const Grid a = find_grid_with_home(kTable, 30, 0);
        const Grid b = find_grid_with_home(kTable, 30, 1);
        const Grid c = find_grid_with_home(kTable, 30, 2);
        const uint32_t sa = insert_slot(keys, a, nullptr);
        insert_slot(keys, b, nullptr);
        evict_slot(keys, sa);
        bool ins = false;
        const uint32_t sc = insert_slot(keys, c, &ins);
        PT_ASSERT(ins && sc == sa, "new key reuses tombstone slot");
        PT_ASSERT(find_slot(keys, b) != kSharcSlotInvalid,
                  "B still findable after tombstone reuse");
    }

    // ── 4. wraparound: home = table-1 で 3 連衝突 ──
    {
        std::vector<uint32_t> keys(kTable, 0u);
        const Grid a = find_grid_with_home(kTable, kTable - 1, 0);
        const Grid b = find_grid_with_home(kTable, kTable - 1, 1);
        const Grid c = find_grid_with_home(kTable, kTable - 1, 2);
        insert_slot(keys, a, nullptr);
        const uint32_t sb = insert_slot(keys, b, nullptr);
        const uint32_t sc = insert_slot(keys, c, nullptr);
        PT_ASSERT(sb == 0u && sc == 1u, "probe wraps to slot 0,1");
        PT_ASSERT(find_slot(keys, b) == sb && find_slot(keys, c) == sc,
                  "wraparound entries findable");
    }

    // ── 5. probe 上限: 満杯チェインで INVALID (無言消失しない) ──
    {
        std::vector<uint32_t> keys(kTable, 0u);
        for (uint32_t i = 0; i < kSharcProbeLimit; ++i) {
            keys[(5 + i) & (kTable - 1)] = 0xABCD0000u + i;   // 他人で埋める
        }
        const Grid g = find_grid_with_home(kTable, 5, 0);
        bool ins = true;
        PT_ASSERT(insert_slot(keys, g, &ins) == kSharcSlotInvalid && !ins,
                  "probe-limit insert returns INVALID");
    }

    // ── 6. 予約値回避 + グリッドパック往復 ──
    {
        // 指紋は 0 / TOMBSTONE を返さない (総当たりサンプル)
        for (int32_t x = -50; x < 50; ++x) {
            const uint32_t fp = sharc_fingerprint(x, x * 3, -x, 1);
            PT_ASSERT(fp != 0u && fp != kTombstone,
                      "fingerprint avoids reserved values");
        }
        // pack/unpack 往復 (負座標含む)
        const std::array<std::array<int32_t, 3>, 4> cases = {{
            {0, 0, 0}, {-1, 1, -1}, {12345, -6789, 400}, {-524287, 524287, 1}}};
        for (const auto& c : cases) {
            for (uint32_t lvl = 0; lvl < 8; ++lvl) {
                const auto p = sharc_pack_grid_level(c[0], c[1], c[2], lvl);
                int32_t gx, gy, gz;
                uint32_t glvl;
                sharc_unpack_grid_level(p, gx, gy, gz, glvl);
                PT_ASSERT(gx == c[0] && gy == c[1] && gz == c[2] &&
                              glvl == lvl,
                          "grid pack roundtrip");
            }
        }
    }

    return pictor_test::report("unit_sharc_hash_test");
}
