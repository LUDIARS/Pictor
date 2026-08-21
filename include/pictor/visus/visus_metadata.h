#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pictor {

class VisusMetadata;

// ============================================================
// VisusValue — Visus metadata が保持する JSON 値
// ============================================================
// null / bool / number / string / array / object の 6 種。 Pictor は
// 構造を解釈しない (`spec/feature/visus-v2-design.md` §2.1)。 配列と
// オブジェクトは深いコピー (値セマンティクス)。

class VisusValue {
public:
    enum class Type : uint8_t { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };
    using Array = std::vector<VisusValue>;

    VisusValue();
    VisusValue(bool v);
    VisusValue(double v);
    VisusValue(int v);
    VisusValue(unsigned v);
    VisusValue(int64_t v);
    VisusValue(uint64_t v);
    VisusValue(const char* v);
    VisusValue(std::string_view v);
    VisusValue(std::string v);
    VisusValue(Array v);
    VisusValue(VisusMetadata v);

    /// move 後の source は null 値になる (中身の無い array/object を名乗らない)。
    VisusValue(const VisusValue& other);
    VisusValue(VisusValue&& other) noexcept;
    VisusValue& operator=(const VisusValue& other);
    VisusValue& operator=(VisusValue&& other) noexcept;
    ~VisusValue();

    Type type() const { return type_; }
    bool is_null()   const { return type_ == Type::NUL; }
    bool is_bool()   const { return type_ == Type::BOOL; }
    bool is_number() const { return type_ == Type::NUMBER; }
    bool is_string() const { return type_ == Type::STRING; }
    bool is_array()  const { return type_ == Type::ARRAY; }
    bool is_object() const { return type_ == Type::OBJECT; }

    /// 型が合わないときは既定値 (false / 0 / 空) を返す。
    bool               as_bool()   const;
    double             as_number() const;
    const std::string& as_string() const;
    const Array&       as_array()  const;
    const VisusMetadata& as_object() const;

    /// 可変アクセス。 型が合わなければその型へ置き換える。
    Array&         mutable_array();
    VisusMetadata& mutable_object();

    bool operator==(const VisusValue& other) const;
    bool operator!=(const VisusValue& other) const { return !(*this == other); }

private:
    void reset_();
    void copy_from_(const VisusValue& other);

    Type                           type_ = Type::NUL;
    bool                           bool_ = false;
    double                         number_ = 0.0;
    std::string                    string_;
    std::unique_ptr<Array>         array_;
    std::unique_ptr<VisusMetadata> object_;
};

// ============================================================
// VisusMetadata — 挿入順を保持する key → VisusValue
// ============================================================
// 未知 key も保持して round-trip する。 同じ key の `set` は上書き。

class VisusMetadata {
public:
    using Entry    = std::pair<std::string, VisusValue>;
    using Entries  = std::vector<Entry>;
    using iterator = Entries::const_iterator;

    VisusMetadata() = default;

    const VisusValue* find(std::string_view key) const;
    bool              has(std::string_view key) const { return find(key) != nullptr; }

    std::optional<std::string>        get_string(std::string_view key) const;
    std::optional<double>             get_number(std::string_view key) const;
    std::optional<bool>               get_bool(std::string_view key) const;
    const VisusValue::Array*          get_array(std::string_view key) const;
    const VisusMetadata*              get_object(std::string_view key) const;

    /// 既存 key は値を置換 (順序は維持)、 無ければ末尾へ追加。
    void set(std::string key, VisusValue value);
    /// `over` の key を上書きマージする。既存 key の位置は維持し、新規 key は
    /// 末尾へ追加する。`over` の件数 × 索引引きの対数時間 (二乗にはならない)。
    void merge_from(const VisusMetadata& over);
    bool erase(std::string_view key);
    void clear() { entries_.clear(); index_.clear(); }

    size_t   size()  const { return entries_.size(); }
    bool     empty() const { return entries_.empty(); }
    iterator begin() const { return entries_.begin(); }
    iterator end()   const { return entries_.end(); }

    bool operator==(const VisusMetadata& other) const;
    bool operator!=(const VisusMetadata& other) const { return !(*this == other); }

private:
    Entries entries_;
    // JSON may contain tens of thousands of keys. Keep ordered storage for
    // round-trip output and a logarithmic lookup index to avoid quadratic parse time.
    std::map<std::string, size_t, std::less<>> index_;
};

} // namespace pictor
