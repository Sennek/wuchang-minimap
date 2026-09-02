#pragma once

//
// json.hpp - the mod's one JSON reader.
//
// Both manifests the mod loads are produced offline by our own Python tools
// (`maps/maps.json` from tools/navmesh/build_map.py, `markers/<chapter>.json` from
// tools/markers) but they live in the user's game folder as plain text, so they still
// have to survive being hand-edited into nonsense. This is therefore a complete (if
// minimal) recursive-descent parser rather than a pattern match: numbers, strings,
// bools, null, arrays and objects, with a depth cap.
//
// It was extracted verbatim from mapdata.cpp when markers.cpp needed the same thing;
// it is header-only and has no dependency on Windows, on UE4SS or on mm::log, which is
// what lets the offline test target (tests/markers_test.cpp) link it.
//

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mjson
{
struct JValue;
using JObject = std::vector<std::pair<std::string, JValue>>;
using JArray = std::vector<JValue>;

struct JValue
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    } kind = Kind::Null;

    bool b = false;
    double num = 0.0;
    std::string str;
    std::shared_ptr<JArray> arr;
    std::shared_ptr<JObject> obj;

    const JValue* find(std::string_view key) const
    {
        if (kind != Kind::Object || !obj)
        {
            return nullptr;
        }
        for (const auto& kv : *obj)
        {
            if (kv.first == key)
            {
                return &kv.second;
            }
        }
        return nullptr;
    }

    double number_or(double fallback) const
    {
        return kind == Kind::Number ? num : fallback;
    }

    std::string string_or(std::string fallback) const
    {
        return kind == Kind::String ? str : fallback;
    }
};

class JParser
{
  public:
    explicit JParser(std::string_view text) : t_(text) {}

    bool parse(JValue& out)
    {
        skip();
        return value(out, 0) && (skip(), p_ >= t_.size());
    }

  private:
    std::string_view t_;
    std::size_t p_ = 0;

    void skip()
    {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\r' || t_[p_] == '\n'))
        {
            ++p_;
        }
    }

    bool lit(std::string_view s)
    {
        if (t_.compare(p_, s.size(), s) == 0)
        {
            p_ += s.size();
            return true;
        }
        return false;
    }

    bool value(JValue& out, int depth)
    {
        if (depth > 32 || p_ >= t_.size())
        {
            return false;
        }
        switch (t_[p_])
        {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"':
            out.kind = JValue::Kind::String;
            return string(out.str);
        case 't':
            out.kind = JValue::Kind::Bool;
            out.b = true;
            return lit("true");
        case 'f':
            out.kind = JValue::Kind::Bool;
            out.b = false;
            return lit("false");
        case 'n':
            out.kind = JValue::Kind::Null;
            return lit("null");
        default:
            return number(out);
        }
    }

    bool string(std::string& out)
    {
        if (p_ >= t_.size() || t_[p_] != '"')
        {
            return false;
        }
        ++p_;
        out.clear();
        while (p_ < t_.size() && t_[p_] != '"')
        {
            if (t_[p_] == '\\' && p_ + 1 < t_.size())
            {
                ++p_;
                switch (t_[p_])
                {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'u':
                    // Not needed for this manifest; skip the code point.
                    p_ += 4;
                    out.push_back('?');
                    break;
                default:
                    out.push_back(t_[p_]);
                    break;
                }
                ++p_;
                continue;
            }
            out.push_back(t_[p_++]);
        }
        if (p_ >= t_.size())
        {
            return false;
        }
        ++p_; // closing quote
        return true;
    }

    bool number(JValue& out)
    {
        const std::size_t start = p_;
        while (p_ < t_.size() && (std::strchr("+-.eE0123456789", t_[p_]) != nullptr))
        {
            ++p_;
        }
        if (p_ == start)
        {
            return false;
        }
        const std::string text{t_.substr(start, p_ - start)};
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (end == text.c_str())
        {
            return false;
        }
        out.kind = JValue::Kind::Number;
        out.num = v;
        return true;
    }

    bool array(JValue& out, int depth)
    {
        ++p_; // '['
        out.kind = JValue::Kind::Array;
        out.arr = std::make_shared<JArray>();
        skip();
        if (p_ < t_.size() && t_[p_] == ']')
        {
            ++p_;
            return true;
        }
        for (;;)
        {
            skip();
            JValue element{};
            if (!value(element, depth + 1))
            {
                return false;
            }
            out.arr->push_back(std::move(element));
            skip();
            if (p_ < t_.size() && t_[p_] == ',')
            {
                ++p_;
                continue;
            }
            break;
        }
        if (p_ >= t_.size() || t_[p_] != ']')
        {
            return false;
        }
        ++p_;
        return true;
    }

    bool object(JValue& out, int depth)
    {
        ++p_; // '{'
        out.kind = JValue::Kind::Object;
        out.obj = std::make_shared<JObject>();
        skip();
        if (p_ < t_.size() && t_[p_] == '}')
        {
            ++p_;
            return true;
        }
        for (;;)
        {
            skip();
            std::string key;
            if (!string(key))
            {
                return false;
            }
            skip();
            if (p_ >= t_.size() || t_[p_] != ':')
            {
                return false;
            }
            ++p_;
            skip();
            JValue v{};
            if (!value(v, depth + 1))
            {
                return false;
            }
            out.obj->emplace_back(std::move(key), std::move(v));
            skip();
            if (p_ < t_.size() && t_[p_] == ',')
            {
                ++p_;
                continue;
            }
            break;
        }
        if (p_ >= t_.size() || t_[p_] != '}')
        {
            return false;
        }
        ++p_;
        return true;
    }
};
} // namespace mjson
