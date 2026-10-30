// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "sheredom/json.h"
#include <string>
#include <string_view>

namespace oscairn::detail {
// A JSON string literal: quotes, backslashes and control characters escaped.
inline std::string quote( std::string_view s )
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for( unsigned char c : s )
    {
        if( c == '"' || c == '\\' ) { result += '\\'; result += static_cast<char>(c); }
        else if( c < 32 ) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}

// The value of an object member, or null when v is not an object or lacks key.
inline const json_value_s* member( const json_value_s* v, std::string_view key )
{
    if( !v || v->type != json_type_object ) return nullptr;
    for( auto* e = static_cast<const json_object_s*>( v->payload )->start; e; e = e->next )
        if( std::string_view( e->name->string, e->name->string_size ) == key ) return e->value;
    return nullptr;
}

// Check encoding and depth before the recursive parser.
inline std::string_view jsonPreflight( std::string_view s, size_t maxDepth )
{
    size_t depth = 0;
    bool quoted = false, escaped = false;
    for( size_t i = 0; i < s.size(); ++i )
    {
        const auto ch = static_cast<unsigned char>(s[i]);
        if( ch >= 128 )
        {
            unsigned n = 0, code = 0, minimum = 0;
            if( ch >= 0xC2 && ch <= 0xDF ) { n = 1; code = ch & 31; minimum = 0x80; }
            else if( ch >= 0xE0 && ch <= 0xEF ) { n = 2; code = ch & 15; minimum = 0x800; }
            else if( ch >= 0xF0 && ch <= 0xF4 ) { n = 3; code = ch & 7; minimum = 0x10000; }
            else return "Invalid UTF-8";
            while( n-- )
            {
                if( ++i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 )
                    return "Invalid UTF-8";
                code = (code << 6) | (static_cast<unsigned char>(s[i]) & 63);
            }
            if( code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) )
                return "Invalid UTF-8";
            escaped = false;
            continue;
        }
        if( quoted )
        {
            if( ch < 32 ) return "Unescaped control character";
            if( escaped ) escaped = false;
            else if( ch == '\\' ) escaped = true;
            else if( ch == '"' ) quoted = false;
        }
        else if( ch == '"' ) quoted = true;
        else if( ch == '[' || ch == '{' )
        {
            if( ++depth > maxDepth ) return "JSON nesting limit exceeded";
        }
        else if( (ch == ']' || ch == '}') && depth ) --depth;
    }
    return {};
}
}
