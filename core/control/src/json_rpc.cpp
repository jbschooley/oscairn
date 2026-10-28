// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/json_rpc.h"
#include "sheredom/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <locale>
#include <sstream>
#include <set>
#include <stdexcept>

namespace oscairn
{
    namespace
    {
        using Value = const json_value_s*;
        struct Failure
        {
            int code;
            std::string message;
            std::string data = "null";
        };
        [[noreturn]] void invalid( const char* message ) { throw Failure{ -32602, message }; }

        std::string_view view( const json_string_s* s ) { return { s->string, s->string_size }; }
        std::string_view string( Value v )
        {
            if( !v || v->type != json_type_string ) invalid( "Expected a string" );
            return view( static_cast<const json_string_s*>( v->payload ) );
        }
        Value member( Value v, std::string_view name )
        {
            if( !v || v->type != json_type_object ) return nullptr;
            for( auto* e = static_cast<const json_object_s*>( v->payload )->start; e; e = e->next )
                if( view( e->name ) == name ) return e->value;
            return nullptr;
        }
        void fields( Value v, std::initializer_list<std::string_view> allowed )
        {
            if( !v || v->type != json_type_object ) invalid( "Expected named parameters" );
            std::set<std::string_view> seen;
            for( auto* e = static_cast<const json_object_s*>( v->payload )->start; e; e = e->next )
                if( !seen.insert( view( e->name ) ).second ||
                    std::find( allowed.begin(), allowed.end(), view( e->name ) ) == allowed.end() )
                    invalid( "Unknown or duplicate parameter field" );
        }
        std::vector<Value> array( Value v, size_t limit )
        {
            if( !v || v->type != json_type_array ) invalid( "Expected an array" );
            const auto* a = static_cast<const json_array_s*>( v->payload );
            if( a->length == 0 || a->length > limit ) invalid( "Array size outside permitted range" );
            std::vector<Value> result;
            result.reserve( a->length );
            for( auto* e = a->start; e; e = e->next ) result.push_back( e->value );
            return result;
        }
        std::string_view numberText( Value v )
        {
            if( !v || v->type != json_type_number ) invalid( "Expected a number" );
            const auto* n = static_cast<const json_number_s*>( v->payload );
            return { n->number, n->number_size };
        }
        double number( Value v )
        {
            const auto s = numberText( v );
            double result = 0;
            // Floating from_chars requires macOS 26 in libc++; retain older targets.
            std::istringstream input{std::string(s)};
            input.imbue(std::locale::classic());
            input >> result;
            if( input.fail() || !input.eof() || !std::isfinite( result ) )
                invalid( "Expected a finite representable number" );
            return result;
        }
        uint32_t integer( Value v, uint32_t maximum = std::numeric_limits<uint32_t>::max() )
        {
            const auto n = number( v );
            if( n < 0 || n > maximum || std::floor( n ) != n ) invalid( "Integer outside permitted range" );
            return static_cast<uint32_t>( n );
        }
        bool boolean( Value v )
        {
            if( !v || ( v->type != json_type_true && v->type != json_type_false ) ) invalid( "Expected a boolean" );
            return v->type == json_type_true;
        }

        // The parser preserves number tokens, so request IDs never pass through double.
        std::string quote( std::string_view s )
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
        std::string encode( double n )
        {
            if( !std::isfinite( n ) ) throw std::runtime_error( "Nonfinite control result" );
            char buffer[64];
            const auto result = std::to_chars( buffer, buffer + sizeof(buffer), n );
            if( result.ec != std::errc{} ) throw std::runtime_error( "Cannot encode number" );
            return { buffer, result.ptr };
        }
        std::string join( const std::vector<std::string>& values )
        {
            std::string result = "[";
            for( const auto& value : values ) { if( result.size() > 1 ) result += ','; result += value; }
            return result + ']';
        }
        std::string error( std::string_view id, const Failure& f )
        {
            return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"error\":{\"code\":" +
                std::to_string(f.code) + ",\"message\":" + quote(f.message) + ",\"data\":" + f.data + "}}";
        }
        NodeId node( Control& c, Value v )
        {
            NodeId id;
            if( v && v->type == json_type_string )
            {
                try { id = c.nodeId( std::string(string(v)) ); }
                catch( const std::invalid_argument& ) { throw Failure{ -32001, "Unknown node" }; }
            }
            else id = integer(v);
            if( !c.hasNode(id) ) throw Failure{ -32001, "Unknown node" };
            return id;
        }
        uint32_t parameter( Control& c, NodeId n, Value v )
        {
            const auto id = integer(v);
            if( !c.hasParameter(n, id) ) throw Failure{ -32002, "Unknown parameter" };
            return id;
        }
        std::string transport( Control& c )
        {
            const auto s = c.transportState();
            return std::string("{\"playing\":") + ( s.playing ? "true" : "false" ) + ",\"tempo\":" + encode(s.tempo) +
                ",\"beat\":" + encode(s.beat) + ",\"clockSource\":" +
                quote(c.clockSource() == Transport::ClockSource::Internal ? "internal" : "midi") + '}';
        }
        std::string invoke( Control& c, std::string_view method, Value p )
        {
            if( method == "session.hello" )
            {
                fields(p, {});
                return R"({"protocolVersion":1,"server":"oscairn","capabilities":["params.list","params.get","params.set","params.text","transport.get","transport.set","notes.send"],"limits":{"messageBytes":65536,"depth":32,"batch":64,"parameterChanges":128}})";
            }
            if( method == "params.list" )
            {
                fields(p, {"node"});
                const auto n = node(c, member(p,"node"));
                std::vector<std::string> values;
                for( const auto& v : c.parameters(n) )
                    values.push_back( "{\"id\":" + std::to_string(v.id) + ",\"name\":" + quote(v.name) +
                        ",\"minimum\":" + encode(v.minimum) + ",\"maximum\":" + encode(v.maximum) +
                        ",\"defaultValue\":" + encode(v.defaultValue) + ",\"stepped\":" + (v.stepped ? "true" : "false") +
                        ",\"flags\":" + std::to_string(v.flags) + '}' );
                return "{\"node\":" + std::to_string(n) + ",\"parameters\":" + join(values) + '}';
            }
            if( method == "params.get" || method == "params.set" )
            {
                const bool set = method == "params.set";
                fields(p, {"values"});
                struct Change { NodeId node; uint32_t param; double value; };
                std::vector<Change> changes;
                std::set<std::pair<NodeId, uint32_t>> addresses;
                // Validate the whole list before submitting anything to plugin queues.
                for( auto v : array(member(p,"values"), JsonRpc::maxParameterChanges) )
                {
                    if( set ) fields(v, {"node","param","value"});
                    else fields(v, {"node","param"});
                    const auto n = node(c, member(v,"node"));
                    const auto id = parameter(c, n, member(v,"param"));
                    if( !addresses.emplace(n,id).second ) invalid("Duplicate parameter address");
                    const auto value = set ? number(member(v,"value")) : 0.0;
                    if( set && (c.parameterInfo(n,id).flags & CLAP_PARAM_IS_READONLY) ) invalid("Parameter is read-only");
                    changes.push_back({n,id,value});
                }
                std::vector<std::string> values;
                for( const auto& change : changes )
                {
                    if( set && !c.setParameter(change.node, change.param, change.value) )
                        throw Failure{ -32005, "Parameter queue full", "{\"accepted\":" + std::to_string(values.size()) +
                            ",\"index\":" + std::to_string(values.size()) + '}' };
                    values.push_back("{\"node\":" + std::to_string(change.node) + ",\"param\":" + std::to_string(change.param) +
                        ",\"value\":" + encode(c.parameter(change.node,change.param)) + '}');
                }
                return "{\"values\":" + join(values) + '}';
            }
            if( method == "params.text" )
            {
                fields(p, {"node","param","value"});
                const auto n = node(c, member(p,"node"));
                const auto id = parameter(c,n,member(p,"param"));
                return "{\"text\":" + quote(c.parameterText(n,id,number(member(p,"value")))) + '}';
            }
            if( method == "transport.get" ) { fields(p, {}); return transport(c); }
            if( method == "transport.set" )
            {
                fields(p, {"playing","tempo","clockSource"});
                const auto play = member(p,"playing"), tempo = member(p,"tempo"), source = member(p,"clockSource");
                if( !play && !tempo && !source ) invalid("No transport fields supplied");
                const bool playing = play ? boolean(play) : false;
                const double bpm = tempo ? number(tempo) : 120;
                if( bpm < 1 || bpm > 1000 ) invalid("Tempo must be between 1 and 1000 BPM");
                const auto clock = source ? string(source) : "internal";
                if( clock != "internal" && clock != "midi" ) invalid("Unknown clock source");
                if( tempo ) c.setTempo(bpm);
                if( play ) c.setPlaying(playing);
                if( source ) c.setClockSource(clock == "internal" ? Transport::ClockSource::Internal : Transport::ClockSource::MidiClock);
                return R"({"accepted":true})";
            }
            if( method == "notes.send" )
            {
                fields(p, {"on","key","velocity","channel"});
                const bool on = boolean(member(p,"on"));
                const auto key = integer(member(p,"key"),127);
                const auto channel = member(p,"channel") ? integer(member(p,"channel"),15) : 0;
                const auto velocity = member(p,"velocity") ? number(member(p,"velocity")) : (on ? 1.0 : 0.0);
                if( velocity < 0 || velocity > 1 ) invalid("Velocity must be between 0 and 1");
                if( !c.note(on,static_cast<int>(key),velocity,static_cast<int>(channel)) )
                    throw Failure{ -32005, "UI note queue full; emergency release scheduled" };
                return R"({"accepted":true})";
            }
            throw Failure{ -32601, "Method not found" };
        }

        std::optional<std::string> request( Control& c, Value v )
        {
            if( v->type != json_type_object ) return error("null", {-32600,"Invalid Request"});
            std::set<std::string_view> seen;
            for( auto* e = static_cast<const json_object_s*>(v->payload)->start; e; e = e->next )
                if( !seen.insert(view(e->name)).second ) return error("null", {-32600,"Duplicate request member"});
            const auto version = member(v,"jsonrpc"), method = member(v,"method"), id = member(v,"id"), params = member(v,"params");
            if( !version || version->type != json_type_string || string(version) != "2.0" ||
                !method || method->type != json_type_string ||
                (id && id->type != json_type_null && id->type != json_type_string && id->type != json_type_number) ||
                (params && params->type != json_type_object && params->type != json_type_array) )
                return error("null", {-32600,"Invalid Request"});
            const std::string token = !id || id->type == json_type_null ? "null" :
                id->type == json_type_string ? quote(string(id)) : std::string(numberText(id));
            json_object_s emptyObject{nullptr,0};
            json_value_s emptyParams{&emptyObject,json_type_object};
            try
            {
                const auto result = invoke(c,string(method),params ? params : &emptyParams);
                if( id ) return "{\"jsonrpc\":\"2.0\",\"id\":" + token + ",\"result\":" + result + '}';
            }
            catch( const Failure& f ) { if( id ) return error(token,f); }
            catch( const std::invalid_argument& e ) { if( id ) return error(token,{-32602,e.what()}); }
            catch( const std::exception& ) { if( id ) return error(token,{-32603,"Internal error"}); }
            return std::nullopt;
        }

        // Bound recursion before entering the vendored recursive parser. Also reject
        // raw control characters and malformed UTF-8, which it does not fully validate.
        void preflight( std::string_view s )
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
                    else throw Failure{-32700,"Invalid UTF-8"};
                    while( n-- )
                    {
                        if( ++i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 )
                            throw Failure{-32700,"Invalid UTF-8"};
                        code = (code << 6) | (static_cast<unsigned char>(s[i]) & 63);
                    }
                    if( code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) )
                        throw Failure{-32700,"Invalid UTF-8"};
                    escaped = false;
                    continue;
                }
                if( quoted )
                {
                    if( ch < 32 ) throw Failure{-32700,"Unescaped control character"};
                    if( escaped ) escaped = false;
                    else if( ch == '\\' ) escaped = true;
                    else if( ch == '"' ) quoted = false;
                }
                else if( ch == '"' ) quoted = true;
                else if( ch == '[' || ch == '{' )
                {
                    if( ++depth > JsonRpc::maxDepth ) throw Failure{-32006,"JSON nesting limit exceeded"};
                }
                else if( (ch == ']' || ch == '}') && depth ) --depth;
            }
        }
    }

    std::optional<std::string> JsonRpc::dispatch( std::string_view message )
    {
        try
        {
            if( message.size() > maxMessageBytes ) throw Failure{-32006,"Message size limit exceeded"};
            preflight(message);
            std::unique_ptr<json_value_s, decltype(&std::free)> root(json_parse(message.data(),message.size()), &std::free);
            if( !root ) throw Failure{-32700,"Parse error"};
            if( root->type != json_type_array ) return request(control_,root.get());
            const auto* batch = static_cast<const json_array_s*>(root->payload);
            if( batch->length == 0 ) throw Failure{-32600,"Empty batch"};
            if( batch->length > maxBatch ) throw Failure{-32006,"Batch limit exceeded"};
            std::vector<std::string> responses;
            for( auto* e = batch->start; e; e = e->next )
                if( auto result = request(control_,e->value) ) responses.push_back(std::move(*result));
            if( responses.empty() ) return std::nullopt;
            return join(responses);
        }
        catch( const Failure& f ) { return error("null",f); }
        catch( const std::exception& ) { return error("null",{-32603,"Internal error"}); }
    }
}
