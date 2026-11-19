// The thread table (hole_standards.h).
#include "hole_standards.h"

#include "../json.h"

#include <cstddef>
#include <exception>
#include <new>
#include <utility>

namespace ray::doc::features {

namespace detail {
// Written by src/doc/CMakeLists.txt from resources/standards/threads.json (thread_table_data.cpp in the build tree).
extern const unsigned char kThreadTableJson[];
extern const std::size_t kThreadTableJsonSize;
} // namespace detail

namespace {

using json::Value;

tl::unexpected<Error> formatError(std::string message)
{
    return tl::unexpected(Error{ErrorCode::FormatError, "Thread table: " + std::move(message), {}});
}

Result<std::string> text(const Value& v, std::string_view key, const std::string& where)
{
    const Value* x = v.find(key);
    if (x == nullptr || !x->isString() || x->asString().empty()) {
        return formatError(where + ": '" + std::string(key) + "' must be a non-empty string");
    }
    return x->asString();
}

Result<double> positive(const Value& v, std::string_view key, const std::string& where)
{
    const Value* x = v.find(key);
    if (x == nullptr || !x->isNumber() || !(x->asNumber() > 0)) {
        return formatError(where + ": '" + std::string(key) + "' must be a number greater than 0");
    }
    return x->asNumber();
}

Result<ThreadSize> readSize(const Value& v, const std::string& where)
{
    if (!v.isObject()) {
        return formatError(where + " must be an object");
    }
    ThreadSize out;
    auto designation = text(v, "designation", where);
    if (!designation) {
        return tl::unexpected(designation.error());
    }
    out.designation = *designation;
    const std::string at = where + " (" + out.designation + ")";
    auto nominal = text(v, "nominal", at);
    auto major = positive(v, "majorDiameter", at);
    auto pitch = positive(v, "pitch", at);
    auto minor = positive(v, "basicMinorDiameter", at);
    if (!nominal || !major || !pitch || !minor) {
        return tl::unexpected(!nominal ? nominal.error()
                              : !major ? major.error()
                              : !pitch ? pitch.error()
                                       : minor.error());
    }
    out.nominal = *nominal;
    out.majorDiameter = *major;
    out.pitch = *pitch;
    out.basicMinorDiameter = *minor;
    if (v.find("tpi") != nullptr) {
        auto tpi = positive(v, "tpi", at);
        if (!tpi) {
            return tl::unexpected(tpi.error());
        }
        out.tpi = *tpi;
    }
    const Value* tap = v.find("tapDrill");
    if (tap == nullptr || !tap->isObject()) {
        return formatError(at + ": 'tapDrill' must be an object");
    }
    auto size = text(*tap, "size", at + " tapDrill");
    auto diameter = positive(*tap, "diameter", at + " tapDrill");
    if (!size || !diameter) {
        return tl::unexpected(!size ? size.error() : diameter.error());
    }
    out.tapDrill = TapDrill{*size, *diameter};
    return out;
}

Result<ThreadTable> readTable(const Value& root)
{
    const Value* series = root.find("series");
    if (series == nullptr || !series->isArray() || series->asArray().empty()) {
        return formatError("'series' must be a non-empty array");
    }
    ThreadTable out;
    for (std::size_t i = 0; i < series->asArray().size(); ++i) {
        const Value& s = series->asArray()[i];
        const std::string where = "series[" + std::to_string(i) + "]";
        if (!s.isObject()) {
            return formatError(where + " must be an object");
        }
        ThreadSeries ts;
        auto id = text(s, "id", where);
        auto name = text(s, "name", where);
        auto system = text(s, "system", where);
        auto standard = text(s, "standard", where);
        if (!id || !name || !system || !standard) {
            return tl::unexpected(!id       ? id.error()
                                  : !name   ? name.error()
                                  : !system ? system.error()
                                            : standard.error());
        }
        ts.id = *id;
        ts.name = *name;
        ts.system = *system;
        ts.standard = *standard;
        const Value* sizes = s.find("sizes");
        if (sizes == nullptr || !sizes->isArray()) {
            return formatError(where + " (" + ts.id + "): 'sizes' must be an array");
        }
        for (std::size_t k = 0; k < sizes->asArray().size(); ++k) {
            auto size = readSize(sizes->asArray()[k], ts.id + ".sizes[" + std::to_string(k) + "]");
            if (!size) {
                return tl::unexpected(size.error());
            }
            ts.sizes.push_back(std::move(*size));
        }
        out.series.push_back(std::move(ts));
    }
    return out;
}

// The parsed shipped file and the table read from it.
struct Shipped {
    Value root;
    std::shared_ptr<const ThreadTable> table;
};

Result<std::shared_ptr<const Shipped>> shipped()
{
    static const Result<std::shared_ptr<const Shipped>> loaded = []() -> Result<std::shared_ptr<const Shipped>> {
        try {
            auto root = json::parse(threadTableText());
            if (!root) {
                return formatError(root.error().message);
            }
            auto table = readTable(*root);
            if (!table) {
                return tl::unexpected(table.error());
            }
            auto out = std::make_shared<Shipped>();
            out->root = std::move(*root);
            out->table = std::make_shared<const ThreadTable>(std::move(*table));
            return std::shared_ptr<const Shipped>(std::move(out));
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while reading the thread table", {}});
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, "Unexpected failure while reading the thread table", {}});
        }
    }();
    return loaded;
}

std::string normalised(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        // U+00D7 MULTIPLICATION SIGN in UTF-8.
        if (static_cast<unsigned char>(c) == 0xC3 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x97) {
            out += 'x';
            ++i;
        } else if (c == ' ' || c == '#') {
            continue;
        } else if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    for (const std::string_view suffix : {"unc", "unf"}) {
        if (out.size() > suffix.size() && out.compare(out.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.resize(out.size() - suffix.size());
        }
    }
    return out;
}

} // namespace

std::string_view threadTableText()
{
    return std::string_view(reinterpret_cast<const char*>(detail::kThreadTableJson), detail::kThreadTableJsonSize);
}

Result<ThreadTable> parseThreadTable(std::string_view text)
{
    try {
        auto root = json::parse(text);
        if (!root) {
            return formatError(root.error().message);
        }
        return readTable(*root);
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while reading a thread table", {}});
    }
}

Result<std::shared_ptr<const ThreadTable>> threadTable()
{
    auto s = shipped();
    if (!s) {
        return tl::unexpected(s.error());
    }
    return (*s)->table;
}

const ThreadSeries* findSeries(const ThreadTable& table, std::string_view series)
{
    for (const ThreadSeries& s : table.series) {
        if (s.id == series) {
            return &s;
        }
    }
    return nullptr;
}

const ThreadSize* findThread(const ThreadTable& table, std::string_view series, std::string_view designation)
{
    const ThreadSeries* s = findSeries(table, series);
    if (s == nullptr) {
        return nullptr;
    }
    const std::string wanted = normalised(designation);
    const ThreadSize* byNominal = nullptr;
    int nominalCount = 0;
    for (const ThreadSize& size : s->sizes) {
        if (normalised(size.designation) == wanted) {
            return &size;
        }
        if (normalised(size.nominal) == wanted) {
            byNominal = &size;
            ++nominalCount;
        }
    }
    return nominalCount == 1 ? byNominal : nullptr;
}

Result<std::string> threadListJson(std::string_view series)
{
    auto s = shipped();
    if (!s) {
        return tl::unexpected(s.error());
    }
    try {
        const Value& root = (*s)->root;
        Value::Array list;
        for (const Value& one : root.find("series")->asArray()) {
            const Value* id = one.find("id");
            if (series.empty() || (id != nullptr && id->asString() == series)) {
                list.push_back(one);
            }
        }
        if (list.empty()) {
            std::string known;
            for (const ThreadSeries& t : (*s)->table->series) {
                known += (known.empty() ? "" : ", ") + t.id;
            }
            return tl::unexpected(
                Error{ErrorCode::NotFound,
                      "There is no thread series '" + std::string(series) + "' (known: " + known + ")",
                      {}});
        }
        Value::Object out;
        out.push_back({"series", Value::array(std::move(list))});
        for (const std::string_view key : {"method", "sources"}) {
            if (const Value* v = root.find(key)) {
                out.push_back({std::string(key), *v});
            }
        }
        return json::write(Value::object(std::move(out)));
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while listing the thread table", {}});
    }
}

} // namespace ray::doc::features
