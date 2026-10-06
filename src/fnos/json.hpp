// ps5-homebrew-ui - A small read-only JSON reader for the music API answers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OpenGL runtime links Mesa's parson, which owns the json_object and
// json_array names, so a second JSON library cannot be linked beside it. This
// reader covers what the API answers need: objects, arrays, strings with the
// \uXXXX escapes, numbers and booleans. It never allocates through an exception
// path and refuses nesting deeper than kMaxDepth, because the text comes off
// the network.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hui::fnos
{

class Json
{
  public:
    Json() = default;

    // The whole answer; false when the text is not one JSON value.
    static bool parse(const std::string &text, Json *out);

    bool null() const
    {
        return type_ == Type::kNull;
    }
    bool array() const
    {
        return type_ == Type::kArray;
    }
    // Members of an object or elements of an array; 0 for anything else.
    std::size_t size() const
    {
        return type_ == Type::kObject ? values_.size() : items_.size();
    }
    // The member of an object, or a null value when it is missing.
    const Json &member(const char *key) const;
    // The element of an array, or a null value past the end.
    const Json &element(std::size_t index) const;
    // The i-th member name of an object.
    const std::string &name(std::size_t index) const;

    std::string text() const;                   // stringified for numbers and booleans
    long long number(long long fallback) const; // an integer, or a numeric string
    bool flag() const;

  private:
    enum class Type : std::uint8_t
    {
        kNull,
        kBool,
        kNumber,
        kString,
        kArray,
        kObject,
    };

    // The recursive-descent reader, in json.cpp. Being a nested class is what
    // lets it fill the nodes it builds.
    struct Reader;

    Type type_ = Type::kNull;
    bool bool_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<Json> items_;       // array elements
    std::vector<std::string> keys_; // object member names
    std::vector<Json> values_;      // object member values
};

} // namespace hui::fnos
