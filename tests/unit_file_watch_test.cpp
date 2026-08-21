/// FileWatchSet — mtime 変更検出 / settle 待ち / 不在→出現。
/// 実時間には依存せず、 now_ms を手で進め、 mtime は last_write_time を直接
/// 書き換えて制御する。

#include "pictor/core/file_watch.h"
#include "test_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pictor;
using namespace pictor_test;

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const char* text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// mtime を明示的に +sec 進める (書き込みだけだと分解能で同値になり得るため)。
void bump_mtime(const fs::path& p, int sec) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    fs::last_write_time(p, t + std::chrono::seconds(sec), ec);
}

void test_change_detection_with_settle() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_file_watch";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path f = dir / "a.profile.json";
    write_file(f, "{}");

    FileWatchSet w;
    w.set_settle_ms(200);
    w.add("");
    constexpr char nul_path_data[] = "ignored\0path";
    w.add(std::string(nul_path_data, sizeof(nul_path_data) - 1));
    w.add(f.generic_string());
    PT_ASSERT_OP(w.size(), ==, size_t{1}, "invalid paths ignored; one entry watched");
    PT_ASSERT(w.contains(f.generic_string()), "contains");
    w.add(f.generic_string());
    PT_ASSERT_OP(w.size(), ==, size_t{1}, "duplicate add ignored");

    // 変更なし → 発火なし
    PT_ASSERT(w.poll_changed(1000).empty(), "no change, no fire");

    // 変更 → settle 前は発火しない
    bump_mtime(f, 10);
    PT_ASSERT(w.poll_changed(2000).empty(), "first detection arms settle timer");
    PT_ASSERT(w.poll_changed(2100).empty(), "within settle window, no fire");

    // settle 経過後に発火、 その後は静かになる
    auto fired = w.poll_changed(2250);
    PT_ASSERT_OP(fired.size(), ==, size_t{1}, "fires after settle");
    PT_ASSERT(fired[0] == f.generic_string(), "fired path");
    PT_ASSERT(w.poll_changed(3000).empty(), "no re-fire without new change");

    // 書き込みが続く間は settle タイマーが仕切り直される
    bump_mtime(f, 10);
    PT_ASSERT(w.poll_changed(4000).empty(), "arm");
    bump_mtime(f, 10);
    PT_ASSERT(w.poll_changed(4150).empty(), "still writing -> re-arm");
    PT_ASSERT(w.poll_changed(4300).empty(), "settle restarted, not elapsed");
    PT_ASSERT_OP(w.poll_changed(4400).size(), ==, size_t{1}, "fires once writes stop");

    fs::remove_all(dir, ec);
}

void test_missing_then_created() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_file_watch2";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path f = dir / "late.spv";

    FileWatchSet w;
    w.set_settle_ms(100);
    w.add(f.generic_string());   // 不在で登録 (mtime = -1 基準)
    PT_ASSERT(w.poll_changed(1000).empty(), "missing stays quiet");

    write_file(f, "spv");
    PT_ASSERT(w.poll_changed(2000).empty(), "creation arms settle");
    PT_ASSERT_OP(w.poll_changed(2150).size(), ==, size_t{1}, "creation fires after settle");

    // 削除も変更として扱う
    fs::remove(f, ec);
    PT_ASSERT(w.poll_changed(3000).empty(), "removal arms settle");
    PT_ASSERT_OP(w.poll_changed(3150).size(), ==, size_t{1}, "removal fires");

    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    test_change_detection_with_settle();
    test_missing_then_created();
    return report("unit_file_watch_test");
}
