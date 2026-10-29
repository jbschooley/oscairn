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
        NodeId node( Control& c, Value v, bool endpoint = false )
        {
            NodeId id;
            if( v && v->type == json_type_string )
            {
                try { id = c.nodeId( std::string(string(v)) ); }
                catch( const std::invalid_argument& ) { throw Failure{ -32001, "Unknown node" }; }
            }
            else id = integer(v);
            if( endpoint && (id == graphInput || id == graphOutput) ) return id;
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
        std::string status(Control& c)
        {
            const auto s = c.sessionStatus();
            std::vector<std::string> midi;
            for(const auto& name : s.midiInputs) midi.push_back(quote(name));
            auto channels=[](const auto& list){std::vector<std::string> result;for(const auto& ch:list)result.push_back("{\"deviceChannel\":"+std::to_string(ch.deviceChannel)+",\"name\":"+quote(ch.name)+",\"available\":"+(ch.available?"true":"false")+'}');return join(result);};
            return "{\"audioReady\":"+std::string(s.audioReady?"true":"false")+",\"audioInputs\":"+channels(s.audioInputs)+",\"audioOutputs\":"+channels(s.audioOutputs)+",\"audioDevice\":" + quote(s.audioDevice) + ",\"midiInputs\":" + join(midi) +
                ",\"sampleRate\":" + encode(s.sampleRate) + ",\"bufferSize\":" + std::to_string(s.bufferSize) +
                ",\"cpuLoad\":" + encode(s.cpuLoad) + ",\"deadlineMisses\":" + std::to_string(s.deadlineMisses) + '}';
        }
        std::string invoke( Control& c, JsonRpc& client, std::string_view method, Value p )
        {
            if( method == "session.hello" )
            {
                fields(p, {});
                return R"({"protocolVersion":1,"server":"oscairn","capabilities":["session.status","params.list","params.get","params.set","params.text","params.gesture","graph.audio-routing","graph.get","graph.add","graph.remove","graph.connect","graph.disconnect","graph.map","graph.unmap","graph.commit","subscribe","unsubscribe","transport.get","transport.set","notes.send"],"limits":{"messageBytes":65536,"depth":32,"batch":64,"parameterChanges":128}})";
            }
            if( method == "subscribe" || method == "unsubscribe" )
            {
                fields(p,{"topics","allChanges"});
                const auto topics = array(member(p,"topics"),4);
                const bool all = member(p,"allChanges") ? boolean(member(p,"allChanges")) : false;
                for( auto topic : topics )
                    if( string(topic) != "params.changed" && string(topic) != "transport" && string(topic) != "graph.changed" && string(topic) != "status" )
                        invalid("Unknown subscription topic");
                for( auto topic : topics ) client.subscription(string(topic),method == "subscribe",all);
                return R"({"accepted":true})";
            }
            if( method == "session.status" ) { fields(p,{}); return status(c); }
            if( method == "graph.get" )
            {
                fields(p,{});
                const auto graph = c.graphState();
                std::vector<std::string> nodes,connections,mappings;
                auto ports = [](const auto& source) {
                    std::vector<std::string> result;
                    for( const auto& port : source )
                        result.push_back("{\"id\":" + std::to_string(port.id) + ",\"name\":" + quote(port.name) +
                            ",\"channels\":" + std::to_string(port.channels) + ",\"flags\":" + std::to_string(port.flags) + '}');
                    return join(result);
                };
                for( const auto& n : graph.topology.nodes )
                    nodes.push_back("{\"id\":" + std::to_string(n.id) + ",\"name\":" + quote(graph.names.at(n.id)) +
                        ",\"plugin\":{\"id\":" + quote(n.plugin.id) + ",\"path\":" + quote(n.plugin.path) + ",\"name\":" + quote(n.plugin.name) +
                        "},\"audioInputs\":" + ports(n.inputs) + ",\"audioOutputs\":" + ports(n.outputs) +
                        ",\"noteInputs\":" + std::to_string(n.noteInputs) + ",\"noteOutputs\":" + std::to_string(n.noteOutputs) + '}');
                auto edges = [&](const auto& list, const char* kind) {
                    for( const auto& [from,to] : list ) connections.push_back("{\"kind\":" + quote(kind) +
                        ",\"from\":" + std::to_string(from) + ",\"to\":" + std::to_string(to) + '}');
                };
                edges(graph.topology.audio,"audio"); edges(graph.topology.notes,"notes");
                for(const auto& r:graph.topology.audioRoutes)
                    connections.push_back("{\"kind\":\"audio\",\"from\":"+std::to_string(r.from.node)+",\"to\":"+std::to_string(r.to.node)+
                        ",\"fromPort\":"+std::to_string(r.from.port)+",\"fromChannel\":"+std::to_string(r.from.channel)+
                        ",\"toPort\":"+std::to_string(r.to.port)+",\"toChannel\":"+std::to_string(r.to.channel)+
                        ",\"channels\":"+std::to_string(r.channels)+",\"gain\":"+encode(r.gain)+'}');
                for( const auto& m : graph.topology.mappings )
                    mappings.push_back("{\"from\":" + std::to_string(m.source) + ",\"lane\":" + std::to_string(m.lane) +
                        ",\"to\":" + std::to_string(m.target) + ",\"param\":" + std::to_string(m.param) + '}');
                return "{\"revision\":" + std::to_string(graph.revision) + ",\"staged\":" + (graph.staged ? "true" : "false") +
                    ",\"input\":" + std::to_string(graphInput) + ",\"output\":" + std::to_string(graphOutput) +
                    ",\"audioInputs\":"+ports(graph.topology.audioLayout.inputs)+",\"audioOutputs\":"+ports(graph.topology.audioLayout.outputs)+
                    ",\"nodes\":" + join(nodes) + ",\"connections\":" + join(connections) + ",\"mappings\":" + join(mappings) + '}';
            }
            if( method == "graph.add" )
            {
                fields(p,{"name","path","pluginId"});
                const auto name = member(p,"name") ? std::string(string(member(p,"name"))) : "";
                const auto path = std::string(string(member(p,"path")));
                const auto plugin = member(p,"pluginId") ? std::string(string(member(p,"pluginId"))) : "";
                if( path.empty() || path.find('\0') != std::string::npos ) invalid("Invalid plugin path");
                try { return "{\"node\":" + std::to_string(c.add(name,path,plugin)) + '}'; }
                catch( const std::runtime_error& e ) { throw Failure{-32004,e.what()}; }
            }
            if( method == "graph.remove" )
            {
                fields(p,{"node"}); c.remove(node(c,member(p,"node")));
                return R"({"accepted":true})";
            }
            if( method == "graph.connect" || method == "graph.disconnect" )
            {
                fields(p,{"kind","from","to","fromPort","fromChannel","toPort","toChannel","channels","gain"});
                const auto kind = string(member(p,"kind"));
                if( kind != "audio" && kind != "notes" ) invalid("Unknown connection kind");
                const auto from = node(c,member(p,"from"),true), to = node(c,member(p,"to"),true);
                const bool explicitRoute=member(p,"fromPort") || member(p,"fromChannel") || member(p,"toPort") || member(p,"toChannel") || member(p,"channels") || member(p,"gain");
                if(explicitRoute && kind!="audio")invalid("Port/channel routing requires an audio connection");
                AudioConnection route{{from,0,0},{to,0,0}};
                if(explicitRoute)
                {
                    route.from.port=integer(member(p,"fromPort"));route.from.channel=integer(member(p,"fromChannel"));
                    route.to.port=integer(member(p,"toPort"));route.to.channel=integer(member(p,"toChannel"));
                    route.channels=member(p,"channels")?integer(member(p,"channels")):1;
                    route.gain=member(p,"gain")?number(member(p,"gain")):1;
                }
                try
                {
                    if(explicitRoute)
                    {if(method=="graph.connect")c.connectAudio(route);else c.disconnectAudio(route);}
                    else if( method == "graph.connect" )
                    {
                        if( kind == "audio" ) c.connectAudio(from,to); else c.connectNotes(from,to);
                    }
                    else { if( kind == "audio" ) c.disconnectAudio(from,to); else c.disconnectNotes(from,to); }
                }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
                return R"({"accepted":true})";
            }
            if( method == "graph.map" || method == "graph.unmap" )
            {
                const bool map = method == "graph.map";
                if( map ) fields(p,{"from","lane","to","param"}); else fields(p,{"from","lane"});
                const auto from = node(c,member(p,"from")), lane = integer(member(p,"lane"));
                const auto to = map ? node(c,member(p,"to")) : 0;
                const auto param = map ? parameter(c,to,member(p,"param")) : 0;
                try { if( map ) c.mapParameter(from,lane,to,param); else c.unmapParameter(from,lane); }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
                return R"({"accepted":true})";
            }
            if( method == "graph.commit" )
            {
                fields(p,{});
                try { c.commit(); }
                catch( const std::runtime_error& e ) { throw Failure{-32003,e.what()}; }
                return "{\"revision\":" + std::to_string(c.graphState().revision) + '}';
            }
            if( method == "params.gesture" )
            {
                fields(p,{"node","param","begin"});
                const auto n = node(c,member(p,"node"));
                const auto id = parameter(c,n,member(p,"param"));
                if( !c.parameterGesture(n,id,boolean(member(p,"begin"))) )
                    throw Failure{-32005,"Pending parameter command store full"};
                return R"({"accepted":true})";
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
                        throw Failure{ -32005, "Pending parameter command store full", "{\"accepted\":" + std::to_string(values.size()) +
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

        std::optional<std::string> request( Control& c, JsonRpc& client, Value v )
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
                const auto result = invoke(c,client,string(method),params ? params : &emptyParams);
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

    void JsonRpc::subscription( std::string_view topic, bool enabled, bool allChanges )
    {
        if( topic == "params.changed" )
        {
            if( enabled && !params_ ) { paramsCursor_ = control_.changeSequence(); lastParams_.reset(); }
            params_ = enabled; allChanges_ = allChanges;
        }
        else if( topic == "transport" )
        {
            if( enabled && !transport_ ) { lastTransport_.reset(); lastTransportValue_.clear(); }
            transport_ = enabled;
        }
        else if( topic == "graph.changed" )
        {
            if( enabled && !graph_ ) graphCursor_ = control_.changeSequence();
            graph_ = enabled;
        }
        else if(topic == "status")
        {
            if(enabled && !status_) { lastStatus_.reset(); lastStatusValue_.clear(); }
            status_ = enabled;
        }
        else throw std::invalid_argument("Unknown subscription topic");
    }

    std::vector<std::string> JsonRpc::notifications( uint64_t now )
    {
        std::vector<std::string> result;
        auto send = [&](std::string_view method, const std::string& params) {
            result.push_back("{\"jsonrpc\":\"2.0\",\"method\":" + quote(method) + ",\"params\":" + params + '}');
        };
        auto due = [&](const auto& last) { return !last || now < *last || now - *last >= 16666667; };
        if( params_ && (allChanges_ || due(lastParams_)) )
        {
            const auto changes = control_.changesAfter(paramsCursor_);
            std::vector<std::string> values;
            std::map<std::pair<NodeId,uint32_t>,Control::Change> latest;
            auto append = [&](const Control::Change& change) {
                const auto kind = change.kind == Control::ChangeKind::Value ? "value" :
                    change.kind == Control::ChangeKind::GestureBegin ? "begin" : "end";
                values.push_back("{\"sequence\":" + std::to_string(change.sequence) + ",\"node\":" + std::to_string(change.node) +
                    ",\"param\":" + std::to_string(change.param) + ",\"kind\":" + quote(kind) +
                    ",\"source\":" + quote(change.plugin ? "plugin" : "control") +
                    (change.kind == Control::ChangeKind::Value ? ",\"value\":" + encode(change.value) : "") + '}');
            };
            auto flush = [&] {
                std::vector<Control::Change> ordered;
                for( const auto& [address,change] : latest ) ordered.push_back(change);
                std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.sequence < b.sequence;});
                for( const auto& change : ordered ) append(change);
                latest.clear();
            };
            bool lost = changes.lost;
            for( const auto& change : changes.events )
            {
                if( change.kind == Control::ChangeKind::Resync ) { lost = true; continue; }
                if( change.kind == Control::ChangeKind::Graph ) continue;
                if( !allChanges_ && change.kind == Control::ChangeKind::Value ) latest[{change.node,change.param}] = change;
                else { flush(); append(change); }
            }
            flush();
            if( lost ) send("session.resync",R"({"topic":"params.changed"})");
            if( !values.empty() ) send("params.changed","{\"changes\":" + join(values) + '}');
            paramsCursor_ = changes.latest;
            lastParams_ = now;
        }
        if( transport_ && due(lastTransport_) )
        {
            auto state = transport(control_);
            if( state != lastTransportValue_ ) { send("transport",state); lastTransportValue_ = std::move(state); }
            lastTransport_ = now;
        }
        if(status_ && due(lastStatus_))
        {
            auto value = status(control_);
            if(value != lastStatusValue_) { send("status",value); lastStatusValue_ = std::move(value); }
            lastStatus_ = now;
        }
        if( graph_ )
        {
            const auto changes = control_.changesAfter(graphCursor_);
            if( changes.lost || std::any_of(changes.events.begin(),changes.events.end(),[](const auto& c){return c.kind == Control::ChangeKind::Graph;}) )
                send("graph.changed","{\"revision\":" + std::to_string(control_.graphState().revision) + '}');
            graphCursor_ = changes.latest;
        }
        return result;
    }

    std::optional<std::string> JsonRpc::dispatch( std::string_view message )
    {
        try
        {
            if( message.size() > maxMessageBytes ) throw Failure{-32006,"Message size limit exceeded"};
            preflight(message);
            std::unique_ptr<json_value_s, decltype(&std::free)> root(json_parse(message.data(),message.size()), &std::free);
            if( !root ) throw Failure{-32700,"Parse error"};
            if( root->type != json_type_array ) return request(control_,*this,root.get());
            const auto* batch = static_cast<const json_array_s*>(root->payload);
            if( batch->length == 0 ) throw Failure{-32600,"Empty batch"};
            if( batch->length > maxBatch ) throw Failure{-32006,"Batch limit exceeded"};
            std::vector<std::string> responses;
            for( auto* e = batch->start; e; e = e->next )
                if( auto result = request(control_,*this,e->value) ) responses.push_back(std::move(*result));
            if( responses.empty() ) return std::nullopt;
            return join(responses);
        }
        catch( const Failure& f ) { return error("null",f); }
        catch( const std::exception& ) { return error("null",{-32603,"Internal error"}); }
    }
}
