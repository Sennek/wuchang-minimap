#pragma once

//
// config_rewrite - rewrite the VALUES in a config file and change nothing else.
//
// Each `key = value` line gets the new value in place of the old one; every byte that is
// not the value stays put - comments (including a `; ...` after a value), blank lines,
// key order, indentation, the UTF-8 BOM, line endings, and keys this build does not know.
// A key absent from the file is appended once at the end under a banner.
//
// A pure function of (text, key -> value): no Windows, no UE4SS, no ImGui, no allocation
// beyond the string it returns, so tests can round-trip the shipped file with the game
// closed.
//
// Line rules are the loader's (mmstate.cpp apply_text / cfgkeys::keys_in): `;` and `#`
// start a comment, the key is everything left of the first `=` in the uncommented part,
// trimmed. A line whose first non-blank character is `;` or `#` is a comment and never a
// key line, even if it contains an `=`.
//

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cfgrw
{
    using Pair = std::pair<std::string, std::string>;

    struct Result
    {
        std::string text;
        int rewritten = 0; // value lines whose key was in the map
        int changed = 0;   // ...of which the value actually differed
        int appended = 0;  // keys that were not in the file at all
    };

    namespace detail
    {
        inline bool is_space(char c)
        {
            return c == ' ' || c == '\t';
        }

        inline std::string_view trim(std::string_view v)
        {
            std::size_t a = 0;
            std::size_t b = v.size();
            while (a < b && (is_space(v[a]) || v[a] == '\r' || v[a] == '\n'))
            {
                ++a;
            }
            while (b > a && (is_space(v[b - 1]) || v[b - 1] == '\r' || v[b - 1] == '\n'))
            {
                --b;
            }
            return v.substr(a, b - a);
        }
    } // namespace detail

    // The value `key` should be written with, or nullptr when the file's line for it
    // must be left alone.
    inline const std::string* lookup(const std::vector<Pair>& kv, std::string_view key)
    {
        for (const Pair& p : kv)
        {
            if (p.first == key)
            {
                return &p.second;
            }
        }
        return nullptr;
    }

    // `banner` is the comment line written above appended keys (without its newline).
    // Pass an empty view to append them with no banner.
    inline Result rewrite(std::string_view text, const std::vector<Pair>& kv, std::string_view banner)
    {
        Result out;
        out.text.reserve(text.size() + 256);

        std::vector<bool> seen(kv.size(), false);

        // The BOM belongs to the file, not to a line: copy it through untouched.
        std::size_t pos = 0;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        {
            out.text.append(text.substr(0, 3));
            pos = 3;
        }

        // The dominant line ending, so an appended block matches the rest of the file.
        const bool crlf = text.find("\r\n") != std::string_view::npos;
        const std::string_view eol = crlf ? std::string_view{"\r\n"} : std::string_view{"\n"};

        bool last_line_empty = true;
        while (pos < text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            const std::size_t line_end = (nl == std::string_view::npos) ? text.size() : nl + 1;
            const std::string_view line = text.substr(pos, line_end - pos);
            pos = line_end;

            // `body` is the line without its terminator; `term` is "\n", "\r\n" or "".
            std::size_t body_len = line.size();
            while (body_len > 0 && (line[body_len - 1] == '\n' || line[body_len - 1] == '\r'))
            {
                --body_len;
            }
            const std::string_view body = line.substr(0, body_len);
            const std::string_view term = line.substr(body_len);
            last_line_empty = detail::trim(body).empty();

            // A comment line, or a line with no `=` before the comment, is copied through.
            std::size_t first = 0;
            while (first < body.size() && detail::is_space(body[first]))
            {
                ++first;
            }
            const bool is_comment = first < body.size() && (body[first] == ';' || body[first] == '#');
            const std::size_t comment_at = body.find_first_of(";#");
            const std::string_view uncommented =
                body.substr(0, comment_at == std::string_view::npos ? body.size() : comment_at);
            const std::size_t eq = uncommented.find('=');
            if (is_comment || eq == std::string_view::npos)
            {
                out.text.append(line);
                continue;
            }
            const std::string_view key = detail::trim(uncommented.substr(0, eq));
            const std::string* want = key.empty() ? nullptr : lookup(kv, key);
            if (want == nullptr)
            {
                // An unknown key, or one this save is not touching: byte for byte.
                out.text.append(line);
                continue;
            }
            for (std::size_t i = 0; i < kv.size(); ++i)
            {
                if (kv[i].first == key)
                {
                    seen[i] = true;
                }
            }

            // Split what follows the `=` into [whitespace][value][the rest]; "the rest"
            // is trailing whitespace plus any inline comment, copied verbatim, so a save
            // that changes nothing is byte-identical.
            std::size_t vs = eq + 1;
            while (vs < uncommented.size() && detail::is_space(uncommented[vs]))
            {
                ++vs;
            }
            std::size_t ve = uncommented.size();
            while (ve > vs && detail::is_space(uncommented[ve - 1]))
            {
                --ve;
            }
            const std::string_view old_value = uncommented.substr(vs, ve - vs);

            out.text.append(body.substr(0, vs)); // everything up to where the value starts
            out.text.append(*want);
            out.text.append(body.substr(ve)); // trailing spaces + any inline comment
            out.text.append(term);

            ++out.rewritten;
            if (old_value != *want)
            {
                ++out.changed;
            }
        }

        // Keys the file does not carry, appended at the end under one banner.
        std::string tail;
        for (std::size_t i = 0; i < kv.size(); ++i)
        {
            if (!seen[i])
            {
                tail.append(kv[i].first);
                tail.append(" = ");
                tail.append(kv[i].second);
                tail.append(eol);
                ++out.appended;
            }
        }
        if (out.appended > 0)
        {
            if (!out.text.empty() && !last_line_empty)
            {
                out.text.append(eol);
            }
            if (!banner.empty())
            {
                out.text.append(banner);
                out.text.append(eol);
            }
            out.text.append(tail);
        }
        return out;
    }

    // The subset of `kv` whose keys satisfy `pred`; splits one config_kv() into the
    // player file's half and the dev file's half.
    template <typename Pred>
    inline std::vector<Pair> filter(const std::vector<Pair>& kv, Pred pred)
    {
        std::vector<Pair> out;
        out.reserve(kv.size());
        for (const Pair& p : kv)
        {
            if (pred(p.first))
            {
                out.push_back(p);
            }
        }
        return out;
    }
} // namespace cfgrw
