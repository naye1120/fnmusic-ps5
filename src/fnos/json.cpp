// ps5-homebrew-ui - JSON reader implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnos/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hui::fnos
{

namespace
{

const Json &no_value()
{
    static const Json missing;
    return missing;
}

} // namespace

// The reader is a nested class so that it may fill the nodes it builds.
struct Json::Reader
{
    // Nesting deeper than this is refused: the text arrives over the network
    // and this recurses once per level.
    static constexpr int kMaxDepth = 64;

    const char *at = nullptr;
    const char *end = nullptr;

    void skip_space()
    {
        while (at != end)
        {
            const char c = *at;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                return;
            ++at;
        }
    }

    bool word(const char *literal)
    {
        const std::size_t length = std::strlen(literal);
        if (static_cast<std::size_t>(end - at) < length || std::memcmp(at, literal, length) != 0)
            return false;
        at += length;
        return true;
    }

    // Appends one UTF-8 code point.
    void emit(std::string *out, unsigned code) const
    {
        if (code < 0x80u)
        {
            out->push_back(static_cast<char>(code));
        }
        else if (code < 0x800u)
        {
            out->push_back(static_cast<char>(0xc0u | (code >> 6)));
            out->push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
        else if (code < 0x10000u)
        {
            out->push_back(static_cast<char>(0xe0u | (code >> 12)));
            out->push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3fu)));
            out->push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
        else
        {
            out->push_back(static_cast<char>(0xf0u | (code >> 18)));
            out->push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3fu)));
            out->push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3fu)));
            out->push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
    }

    bool hex(unsigned *value)
    {
        if (end - at < 4)
            return false;
        unsigned code = 0;
        for (int digit = 0; digit < 4; ++digit)
        {
            const char c = at[digit];
            unsigned part = 0;
            if (c >= '0' && c <= '9')
                part = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                part = static_cast<unsigned>(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F')
                part = static_cast<unsigned>(c - 'A') + 10u;
            else
                return false;
            code = code * 16u + part;
        }
        at += 4;
        *value = code;
        return true;
    }

    bool string(std::string *out)
    {
        if (at == end || *at != '"')
            return false;
        ++at;
        while (at != end)
        {
            const char c = *at;
            if (c == '"')
            {
                ++at;
                return true;
            }
            ++at;
            if (c != '\\')
            {
                out->push_back(c);
                continue;
            }
            if (at == end)
                return false;
            const char escape = *at++;
            switch (escape)
            {
            case '"':
                out->push_back('"');
                break;
            case '\\':
                out->push_back('\\');
                break;
            case '/':
                out->push_back('/');
                break;
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'u':
            {
                unsigned code = 0;
                if (!hex(&code))
                    return false;
                if (code >= 0xd800u && code < 0xdc00u)
                {
                    // A surrogate pair carries one astral code point.
                    if (end - at < 2 || at[0] != '\\' || at[1] != 'u')
                        return false;
                    at += 2;
                    unsigned low = 0;
                    if (!hex(&low) || low < 0xdc00u || low >= 0xe000u)
                        return false;
                    code = 0x10000u + ((code - 0xd800u) << 10) + (low - 0xdc00u);
                }
                emit(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool value(Json *out, int depth)
    {
        if (depth > kMaxDepth)
            return false;
        skip_space();
        if (at == end)
            return false;
        const char c = *at;
        if (c == '{')
        {
            ++at;
            skip_space();
            if (at != end && *at == '}')
            {
                ++at;
                out->type_ = Json::Type::kObject;
                return true;
            }
            for (;;)
            {
                skip_space();
                std::string key;
                if (!string(&key))
                    return false;
                skip_space();
                if (at == end || *at != ':')
                    return false;
                ++at;
                Json child;
                if (!value(&child, depth + 1))
                    return false;
                out->keys_.push_back(std::move(key));
                out->values_.push_back(std::move(child));
                skip_space();
                if (at == end)
                    return false;
                if (*at == ',')
                {
                    ++at;
                    continue;
                }
                if (*at != '}')
                    return false;
                ++at;
                out->type_ = Json::Type::kObject;
                return true;
            }
        }
        if (c == '[')
        {
            ++at;
            skip_space();
            if (at != end && *at == ']')
            {
                ++at;
                out->type_ = Json::Type::kArray;
                return true;
            }
            for (;;)
            {
                Json child;
                if (!value(&child, depth + 1))
                    return false;
                out->items_.push_back(std::move(child));
                skip_space();
                if (at == end)
                    return false;
                if (*at == ',')
                {
                    ++at;
                    continue;
                }
                if (*at != ']')
                    return false;
                ++at;
                out->type_ = Json::Type::kArray;
                return true;
            }
        }
        if (c == '"')
        {
            out->type_ = Json::Type::kString;
            return string(&out->text_);
        }
        if (c == 't')
        {
            if (!word("true"))
                return false;
            out->type_ = Json::Type::kBool;
            out->bool_ = true;
            return true;
        }
        if (c == 'f')
        {
            if (!word("false"))
                return false;
            out->type_ = Json::Type::kBool;
            out->bool_ = false;
            return true;
        }
        if (c == 'n')
        {
            if (!word("null"))
                return false;
            out->type_ = Json::Type::kNull;
            return true;
        }
        // A number: strtod is the shortest correct way, and the text is ours.
        char *stop = nullptr;
        const double parsed = std::strtod(at, &stop);
        if (stop == at || stop == nullptr)
            return false;
        at = stop;
        out->type_ = Json::Type::kNumber;
        out->number_ = parsed;
        return true;
    }
};

bool Json::parse(const std::string &text, Json *out)
{
    Reader reader;
    reader.at = text.c_str();
    reader.end = text.c_str() + text.size();
    Json value;
    if (!reader.value(&value, 0))
        return false;
    reader.skip_space();
    if (reader.at != reader.end)
        return false; // trailing junk is not one value
    *out = std::move(value);
    return true;
}

const Json &Json::member(const char *key) const
{
    if (type_ != Type::kObject)
        return no_value();
    for (std::size_t index = 0; index < keys_.size(); ++index)
    {
        if (keys_[index] == key)
            return values_[index];
    }
    return no_value();
}

const Json &Json::element(std::size_t index) const
{
    if (type_ != Type::kArray || index >= items_.size())
        return no_value();
    return items_[index];
}

const std::string &Json::name(std::size_t index) const
{
    if (type_ != Type::kObject || index >= keys_.size())
        return no_value().text_;
    return keys_[index];
}

std::string Json::text() const
{
    switch (type_)
    {
    case Type::kString:
        return text_;
    case Type::kBool:
        return bool_ ? "true" : "false";
    case Type::kNumber:
    {
        char digits[32];
        std::snprintf(digits, sizeof(digits), "%lld", static_cast<long long>(number_));
        return digits;
    }
    case Type::kNull:
    case Type::kArray:
    case Type::kObject:
        break;
    }
    return {};
}

long long Json::number(long long fallback) const
{
    if (type_ == Type::kNumber)
        return static_cast<long long>(number_);
    if (type_ == Type::kBool)
        return bool_ ? 1 : 0;
    if (type_ == Type::kString && !text_.empty())
    {
        char *stop = nullptr;
        const long long parsed = std::strtoll(text_.c_str(), &stop, 10);
        if (stop != text_.c_str())
            return parsed;
    }
    return fallback;
}

bool Json::flag() const
{
    if (type_ == Type::kBool)
        return bool_;
    if (type_ == Type::kNumber)
        return number_ != 0.0;
    if (type_ == Type::kString)
        return text_ == "true" || text_ == "1";
    return false;
}

} // namespace hui::fnos
