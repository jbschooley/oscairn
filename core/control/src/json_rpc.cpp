// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/json_rpc.h"
#include "JsonInput.h"
#include "PropertyWire.h"
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
        using detail::member;
        void fields( Value v, std::initializer_list<std::string_view> allowed )
        {
            if( !v || v->type != json_type_object ) invalid( "Expected named parameters" );
            std::set<std::string_view> seen;
            for( auto* e = static_cast<const json_object_s*>( v->payload )->start; e; e = e->next )
                if( !seen.insert( view( e->name ) ).second ||
                    std::find( allowed.begin(), allowed.end(), view( e->name ) ) == allowed.end() )
                    invalid( "Unknown or duplicate parameter field" );
        }
        std::vector<Value> array( Value v, size_t limit, size_t minimum=1 )
        {
            if( !v || v->type != json_type_array ) invalid( "Expected an array" );
            const auto* a = static_cast<const json_array_s*>( v->payload );
            if( a->length < minimum || a->length > limit ) invalid( "Array size outside permitted range" );
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
        using detail::quote;
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
            for(const auto& e : s.midiInputs)
                midi.push_back("{\"source\":" + std::to_string(e.source) + ",\"name\":" + quote(e.name) + ",\"kind\":\"" + endpointKinds[size_t(e.kind)] + "\",\"port\":" + std::to_string(e.port) + '}');
            auto channels=[](const auto& list){std::vector<std::string> result;for(const auto& ch:list)result.push_back("{\"deviceChannel\":"+std::to_string(ch.deviceChannel)+",\"name\":"+quote(ch.name)+",\"available\":"+(ch.available?"true":"false")+'}');return join(result);};
            return "{\"audioReady\":"+std::string(s.audioReady?"true":"false")+",\"audioInputs\":"+channels(s.audioInputs)+",\"audioOutputs\":"+channels(s.audioOutputs)+",\"audioDevice\":" + quote(s.audioDevice) + ",\"midiInputs\":" + join(midi) +
                ",\"sampleRate\":" + encode(s.sampleRate) + ",\"bufferSize\":" + std::to_string(s.bufferSize) +
                ",\"cpuLoad\":" + encode(s.cpuLoad) + ",\"deadlineMisses\":" + std::to_string(s.deadlineMisses) + ",\"graphEventDrops\":" + std::to_string(s.graphEventDrops) + ",\"midiEventDrops\":" + std::to_string(s.midiEventDrops) +
                ",\"failures\":" + [&]{ std::vector<std::string> f; for(const auto& x : s.failures) f.push_back("{\"node\":" + std::to_string(x.node) + ",\"name\":" + quote(x.name) + ",\"reason\":" + quote(x.reason) + '}'); return join(f); }() + '}';
        }
        // RFC 0011 parts and wires, as graph.get reports them.
        std::string zonesJson( const std::vector<NoteZone>& zones )
        {
            std::vector<std::string> out;
            for( const auto& z : zones )
                out.push_back("{\"keys\":[" + std::to_string(z.lowKey) + "," + std::to_string(z.highKey) + "],\"velocities\":[" + encode(z.lowVelocity) + "," + encode(z.highVelocity) +
                    "],\"channels\":" + std::to_string(z.channels) + ",\"transpose\":" + std::to_string(z.transpose) + ",\"curve\":" + encode(z.curve) + ",\"fixedVelocity\":" + encode(z.fixedVelocity) + '}');
            return join(out);
        }
        std::vector<NoteZone> zonesFrom( Value v )
        {
            if( !v || v->type != json_type_array ) invalid("zones is an array");
            std::vector<NoteZone> zones;
            for( auto* e = static_cast<const json_array_s*>(v->payload)->start; e; e = e->next )
            {
                if( zones.size() == NoteZones::capacity ) invalid("At most eight zones");
                fields(e->value,{"keys","velocities","channels","transpose","curve","fixedVelocity"});
                NoteZone z;
                auto range = [&](Value r, auto&& read) {
                    if( !r || r->type != json_type_array || static_cast<const json_array_s*>(r->payload)->length != 2 ) invalid("A range is [low, high]");
                    const auto* first = static_cast<const json_array_s*>(r->payload)->start;
                    return std::pair{read(first->value), read(first->next->value)};
                };
                if( const auto k = member(e->value,"keys") ) { const auto [lo,hi] = range(k,[](Value x){return integer(x,127);}); z.lowKey = uint8_t(lo); z.highKey = uint8_t(hi); }
                if( const auto vel = member(e->value,"velocities") ) { const auto [lo,hi] = range(vel,[](Value x){return number(x);}); z.lowVelocity = lo; z.highVelocity = hi; }
                if( member(e->value,"channels") ) z.channels = uint16_t(integer(member(e->value,"channels"),0xFFFF));
                if( member(e->value,"transpose") ) { const auto t = number(member(e->value,"transpose")); if( t < -127 || t > 127 || t != std::floor(t) ) invalid("transpose is -127..127"); z.transpose = int8_t(t); }
                if( member(e->value,"curve") ) z.curve = number(member(e->value,"curve"));
                if( member(e->value,"fixedVelocity") ) z.fixedVelocity = number(member(e->value,"fixedVelocity"));
                if( !z.valid() ) invalid("Invalid zone");
                zones.push_back(z);
            }
            return zones;
        }
        std::string devicesJson( const std::vector<DeviceRule>& rules )
        {
            std::vector<std::string> out;
            for( const auto& r : rules )
                out.push_back(std::string("{\"kind\":\"") + deviceRuleKinds[size_t(r.kind)] + "\",\"port\":" + std::to_string(r.port) + ",\"pattern\":" + quote(r.pattern) + ",\"match\":\"" + deviceMatches[size_t(r.match)] + "\"}");
            return join(out);
        }
        std::vector<DeviceRule> devicesFrom( Value v )
        {
            if( !v || v->type != json_type_array ) invalid("devices is an array");
            std::vector<DeviceRule> rules;
            auto pick = [](const auto& names, std::string_view name, const char* error) {
                const auto at = std::find_if(names.begin(), names.end(), [&](const char* n) { return name == n; });
                if( at == names.end() ) invalid(error);
                return size_t(at - names.begin());
            };
            for( auto* e = static_cast<const json_array_s*>(v->payload)->start; e; e = e->next )
            {
                if( rules.size() == 16 ) invalid("At most 16 device rules");
                fields(e->value,{"kind","port","pattern","match"});
                DeviceRule r;
                r.kind = DeviceRule::Kind(pick(deviceRuleKinds, string(member(e->value,"kind")), "Unknown device rule kind"));
                if( member(e->value,"port") ) r.port = integer(member(e->value,"port"));
                if( member(e->value,"pattern") ) r.pattern = string(member(e->value,"pattern"));
                if( member(e->value,"match") ) r.match = DeviceRule::Match(pick(deviceMatches, string(member(e->value,"match")), "match is equals, contains or regex"));
                if( !r.valid() ) invalid("Invalid device rule");
                rules.push_back(r);
            }
            return rules;
        }
        PartInput inputFrom( Value in )
        {
            fields(in,{"kind","port","zones","devices"});
            PartInput input;
            const auto kind = string(member(in,"kind"));
            input.kind = kind == "notes" ? PartInput::Kind::Notes : kind == "audio" ? PartInput::Kind::Audio : kind == "none" ? PartInput::Kind::None : kind == "bus" ? PartInput::Kind::Bus : (invalid("input kind is notes, audio, none or bus"), PartInput::Kind::None);
            input.port = member(in,"port") ? integer(member(in,"port")) : 0;
            if( member(in,"zones") ) input.zones = zonesFrom(member(in,"zones"));
            if( member(in,"devices") ) input.devices = devicesFrom(member(in,"devices"));
            return input;
        }
        const char* pressureName( PadAssignment::Pressure p ) { return p == PadAssignment::Pressure::Poly ? "poly" : p == PadAssignment::Pressure::Channel ? "channel" : "none"; }
        std::string padsJson( const PadMap& map )
        {
            std::vector<std::string> banks;
            for( const auto& bank : map.assignments )
            {
                std::vector<std::string> pads;
                for( const auto& a : bank ) pads.push_back("{\"note\":" + std::to_string(a.note) + ",\"channel\":" + std::to_string(a.channel) + ",\"velocity\":" + encode(a.velocity) + ",\"pressure\":\"" + pressureName(a.pressure) + "\"}");
                banks.push_back(join(pads));
            }
            return "{\"bank\":" + std::to_string(map.bank) + ",\"banks\":" + join(banks) + '}';
        }
        PadMap padsFrom( Value v )
        {
            fields(v,{"bank","banks"});
            PadMap map;
            map.bank = uint8_t(integer(member(v,"bank"),PadMap::banks - 1));
            const auto banks = member(v,"banks");
            if( !banks || banks->type != json_type_array || static_cast<const json_array_s*>(banks->payload)->length != PadMap::banks ) invalid("banks is eight arrays of 16 pads");
            size_t b = 0;
            for( auto* e = static_cast<const json_array_s*>(banks->payload)->start; e; e = e->next, ++b )
            {
                if( !e->value || e->value->type != json_type_array || static_cast<const json_array_s*>(e->value->payload)->length != PadMap::pads ) invalid("A bank is 16 pads");
                size_t i = 0;
                for( auto* q = static_cast<const json_array_s*>(e->value->payload)->start; q; q = q->next, ++i )
                {
                    fields(q->value,{"note","channel","velocity","pressure"});
                    auto& a = map.assignments[b][i];
                    a.note = uint8_t(integer(member(q->value,"note"),127));
                    const auto c = number(member(q->value,"channel")); if( c < -1 || c > 15 || c != std::floor(c) ) invalid("A pad channel is -1..15");
                    a.channel = int8_t(c);
                    a.velocity = number(member(q->value,"velocity"));
                    const auto p = string(member(q->value,"pressure"));
                    a.pressure = p == "poly" ? PadAssignment::Pressure::Poly : p == "channel" ? PadAssignment::Pressure::Channel : p == "none" ? PadAssignment::Pressure::None : (invalid("pressure is none, poly or channel"), PadAssignment::Pressure::None);
                }
            }
            if( !map.valid() ) invalid("Invalid pad map");
            return map;
        }
        const char* kindName( MacroTarget::Kind k ) { return k == MacroTarget::Kind::Parameter ? "parameter" : k == MacroTarget::Kind::PartLevel ? "partLevel" : k == MacroTarget::Kind::PartMute ? "partMute" : "control"; }
        const char* curveName( MacroTarget::Curve c ) { return c == MacroTarget::Curve::Linear ? "linear" : c == MacroTarget::Curve::Exponential ? "exponential" : c == MacroTarget::Curve::Logarithmic ? "logarithmic" : "s-curve"; }
        std::string macrosJson( const std::vector<Macro>& macros )
        {
            std::vector<std::string> out;
            for( const auto& m : macros )
            {
                std::vector<std::string> targets;
                for( const auto& t : m.targets )
                    targets.push_back(std::string("{\"kind\":\"") + kindName(t.kind) + "\",\"node\":" + std::to_string(t.node) + ",\"param\":" + std::to_string(t.param) +
                        ",\"part\":" + std::to_string(t.part) + ",\"control\":" + std::to_string(t.macro) + ",\"start\":" + encode(t.start) + ",\"end\":" + encode(t.end) +
                        ",\"curve\":\"" + curveName(t.curve) + "\",\"relative\":" + (t.relative ? "true" : "false") + ",\"base\":" + encode(t.base) + '}');
                out.push_back("{\"id\":" + std::to_string(m.id) + ",\"name\":" + quote(m.name) + ",\"value\":" + encode(m.value) + ",\"targets\":" + join(targets) + '}');
            }
            return join(out);
        }
        std::vector<MacroTarget> targetsFrom( Control& c, Value list )
        {
            if( !list || list->type != json_type_array ) invalid("targets is an array");
            std::vector<MacroTarget> targets;
            for( auto* e = static_cast<const json_array_s*>(list->payload)->start; e; e = e->next )
            {
                fields(e->value,{"kind","node","param","part","control","start","end","curve","relative","base"});
                MacroTarget t;
                const auto kind = string(member(e->value,"kind"));
                t.kind = kind == "parameter" ? MacroTarget::Kind::Parameter : kind == "partLevel" ? MacroTarget::Kind::PartLevel : kind == "partMute" ? MacroTarget::Kind::PartMute : kind == "control" ? MacroTarget::Kind::Macro : (invalid("kind is parameter, partLevel, partMute or control"), MacroTarget::Kind::Parameter);
                if( t.kind == MacroTarget::Kind::Parameter ) { t.node = node(c,member(e->value,"node")); t.param = integer(member(e->value,"param")); }
                if( t.kind == MacroTarget::Kind::PartLevel || t.kind == MacroTarget::Kind::PartMute ) t.part = integer(member(e->value,"part"));
                if( t.kind == MacroTarget::Kind::Macro ) t.macro = integer(member(e->value,"control"));
                if( member(e->value,"start") ) t.start = number(member(e->value,"start"));
                if( member(e->value,"end") ) t.end = number(member(e->value,"end"));
                if( const auto curve = member(e->value,"curve") )
                {
                    const auto name = string(curve);
                    t.curve = name == "linear" ? MacroTarget::Curve::Linear : name == "exponential" ? MacroTarget::Curve::Exponential : name == "logarithmic" ? MacroTarget::Curve::Logarithmic : name == "s-curve" ? MacroTarget::Curve::SCurve : (invalid("curve is linear, exponential, logarithmic or s-curve"), MacroTarget::Curve::Linear);
                }
                if( member(e->value,"relative") ) t.relative = boolean(member(e->value,"relative"));
                targets.push_back(t);
            }
            return targets;
        }
        std::string scenesJson( const Control::GraphState& g )
        {
            std::vector<std::string> scenes, parameters;
            for( const auto& s : g.scenes )
            {
                std::vector<std::string> parts, values;
                for( const auto& p : s.parts ) parts.push_back("{\"part\":" + std::to_string(p.part) + ",\"level\":" + encode(p.level) + ",\"mute\":" + (p.mute ? "true" : "false") + '}');
                for( const auto& v : s.values ) values.push_back("{\"node\":" + std::to_string(v.at.node) + ",\"param\":" + std::to_string(v.at.param) + ",\"value\":" + encode(v.value) + '}');
                std::vector<std::string> macros;
                for( const auto& [m, v] : s.macros ) macros.push_back("{\"control\":" + std::to_string(m) + ",\"value\":" + encode(v) + '}');
                scenes.push_back("{\"id\":" + std::to_string(s.id) + ",\"name\":" + quote(s.name) + ",\"parts\":" + join(parts) + ",\"values\":" + join(values) + ",\"controls\":" + join(macros) + '}');
            }
            for( const auto& at : g.sceneParameters ) parameters.push_back("{\"node\":" + std::to_string(at.node) + ",\"param\":" + std::to_string(at.param) + '}');
            return ",\"scenes\":" + join(scenes) + ",\"sceneParameters\":" + join(parameters) + ",\"scene\":" + std::to_string(g.scene) + ",\"controls\":" + macrosJson(g.macros);
        }
        std::string wireJson( const Wire& w )
        {
            std::vector<std::string> pairs;
            for( const auto& [a,b] : w.channels ) pairs.push_back("[" + std::to_string(a) + "," + std::to_string(b) + "]");
            return "{\"id\":" + std::to_string(w.id) + ",\"kind\":" + (w.kind == WireKind::Audio ? "\"audio\"" : "\"notes\"") +
                ",\"owner\":" + (w.owner == WireOwner::Part ? "\"part\",\"part\":" + std::to_string(w.part) : std::string("\"user\"")) +
                ",\"from\":" + std::to_string(w.from) + ",\"to\":" + std::to_string(w.to) + ",\"fromPort\":" + std::to_string(w.fromPort) +
                ",\"toPort\":" + std::to_string(w.toPort) + ",\"channels\":" + join(pairs) + ",\"level\":" + encode(w.level) + ",\"pan\":" + encode(w.pan) +
                ",\"mute\":" + (w.mute ? "true" : "false") + ",\"solo\":" + (w.solo ? "true" : "false") + ",\"zones\":" + zonesJson(w.zones) + ",\"send\":" + std::to_string(w.send) + ",\"split\":" + std::to_string(w.split) + ",\"lane\":" + std::to_string(w.lane) + ",\"fader\":" + (w.fader ? "true" : "false") + ",\"connected\":" + (w.connected ? "true" : "false") + '}';
        }
        const char* inputKind( PartInput::Kind k ) { return k == PartInput::Kind::Notes ? "notes" : k == PartInput::Kind::Audio ? "audio" : k == PartInput::Kind::Bus ? "bus" : "none"; }
        // A chain's entries: node IDs, and {"split":id} for a split.
        std::string chainJson( const std::vector<NodeId>& entries )
        {
            std::vector<std::string> chain;
            for( const auto n : entries ) chain.push_back(isSplit(n) ? "{\"split\":" + std::to_string(n & ~splitMark) + '}' : std::to_string(n));
            return join(chain);
        }
        std::string splitsJson( const std::vector<Split>& splits )
        {
            std::vector<std::string> out;
            for( const auto& s : splits )
            {
                std::vector<std::string> lanes;
                for( const auto& l : s.lanes )
                    lanes.push_back("{\"chain\":" + chainJson(l.chain) + ",\"zones\":" + zonesJson(l.zones) + ",\"level\":" + encode(l.level) + ",\"pan\":" + encode(l.pan) +
                        ",\"mute\":" + (l.mute ? "true" : "false") + ",\"solo\":" + (l.solo ? "true" : "false") + '}');
                out.push_back("{\"id\":" + std::to_string(s.id) + ",\"kind\":\"" + splitKinds[size_t(s.kind)] + "\",\"splitter\":" + std::to_string(s.splitter) +
                    ",\"joiner\":" + std::to_string(s.joiner) + ",\"lanes\":" + join(lanes) + '}');
            }
            return join(out);
        }
        ChainPlace placeFrom( Value p )
        {
            ChainPlace place;
            if( member(p,"split") ) place.split = integer(member(p,"split"));
            if( member(p,"lane") ) place.lane = integer(member(p,"lane"));
            return place;
        }
        std::string partJson( const Part& p )
        {
            const auto chain = chainJson(p.chain);
            return "{\"id\":" + std::to_string(p.id) + ",\"name\":" + quote(p.name) + ",\"input\":{\"kind\":\"" + inputKind(p.input.kind) +
                "\",\"port\":" + std::to_string(p.input.port) + ",\"zones\":" + zonesJson(p.input.zones) + ",\"devices\":" + devicesJson(p.input.devices) + "},\"chain\":" + chain + ",\"splits\":" + splitsJson(p.splits) + ",\"output\":" + std::to_string(p.output) +
                ",\"level\":" + encode(p.level) + ",\"pan\":" + encode(p.pan) + ",\"mute\":" + (p.mute ? "true" : "false") + ",\"solo\":" + (p.solo ? "true" : "false") + ",\"bus\":" + std::to_string(p.bus) + ",\"sends\":" + [&]{ std::vector<std::string> v; for( const auto& s : p.sends ) v.push_back("{\"bus\":" + std::to_string(s.bus) + ",\"level\":" + encode(s.level) + '}'); return join(v); }() + '}';
        }

        std::string invoke( Control& c, JsonRpc& client, std::string_view method, Value p )
        {
            if( method == "session.hello" )
            {
                fields(p, {});
                return R"({"protocolVersion":1,"server":"oscairn","capabilities":["performance.save","performance.load","state.info","state.save","presets.list","properties.list","properties.get","properties.set","properties.request","sounds.get","sounds.prepare","sounds.select","sounds.discard","assets.register","assets.import","assets.libraries","session.status","params.list","params.get","params.set","params.text","params.gesture","graph.audio-routing","graph.get","graph.add","effects.create","effects.resources","graph.remove","graph.replace","graph.connect","graph.disconnect","graph.map","graph.unmap","graph.commit","parts.add","parts.remove","parts.insert","parts.removeNode","parts.move","parts.addSplit","parts.removeSplit","parts.addLane","parts.removeLane","parts.lane","parts.input","parts.output","parts.send","parts.level","parts.name","parts.solo","parts.pan","parts.meters","edit.undo","edit.redo","edit.history","wires.connect","wires.disconnect","wires.mix","wires.zones","scenes.add","scenes.store","scenes.select","scenes.remove","scenes.name","scenes.parameters","controls.add","controls.remove","controls.name","controls.targets","controls.set","subscribe","unsubscribe","transport.get","transport.set","notes.send","pads.get","pads.set","pads.play","pads.pressure"],"limits":{"messageBytes":65536,"depth":32,"batch":64,"parameterChanges":128}})";
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
            if(method=="performance.save" || method=="performance.load") {
                fields(p,{"directory"});const auto directory=std::string(string(member(p,"directory")));
                try{if(method=="performance.save")c.savePerformance(directory);else c.loadPerformance(directory);}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return method=="performance.save"?R"({"saved":true})":R"({"loaded":true})";
            }
            if(method=="state.info") {
                fields(p,{"node"});return std::string("{\"supported\":")+(c.supportsState(node(c,member(p,"node")))?"true}":"false}");
            }
            if(method=="state.save") {
                fields(p,{"node","directory"});const auto n=node(c,member(p,"node"));
                try{c.saveState(n,std::string(string(member(p,"directory"))));}
                catch(const std::runtime_error& e){throw Failure{-32005,e.what()};}
                return R"({"saved":true})";
            }
            if(method=="presets.list") {
                fields(p,{"node"});std::vector<std::string> values;
                for(const auto& v:c.presets(node(c,member(p,"node"))))values.push_back("{\"uri\":"+quote(v.uri)+",\"name\":"+quote(v.name)+'}');
                return "{\"presets\":"+join(values)+'}';
            }
            if(method=="properties.list") {
                fields(p,{"node"});std::vector<std::string> values;
                for(const auto& v:c.properties(node(c,member(p,"node"))))values.push_back("{\"uri\":"+quote(v.uri)+",\"name\":"+quote(v.name)+",\"type\":"+quote(property_wire::typeName(v.type))+",\"readable\":"+(v.readable?"true":"false")+",\"writable\":"+(v.writable?"true":"false")+'}');
                return "{\"properties\":"+join(values)+'}';
            }
            if(method=="properties.get" || method=="properties.request" || method=="properties.set") {
                if(method=="properties.set")fields(p,{"node","uri","value"});else fields(p,{"node","uri"});
                const auto n=node(c,member(p,"node"));const auto uri=std::string(string(member(p,"uri")));
                const auto properties=c.properties(n);const auto found=std::find_if(properties.begin(),properties.end(),[&](const auto& v){return v.uri==uri;});
                if(found==properties.end())invalid("Unknown property URI");
                if(method=="properties.get") {const auto v=c.propertyValue(n,uri);return "{\"value\":"+(v?property_wire::encode(*v,quote,encode):"null")+'}';}
                if(method=="properties.request" && !found->readable)invalid("Property is not readable");
                if(method=="properties.set" && !found->writable)invalid("Property is read-only");
                const bool ok=method=="properties.set"?c.setProperty(n,uri,property_wire::decode(member(p,"value"))):c.requestProperty(n,uri);
                if(!ok)throw Failure{-32005,"Property request rejected (type, size, capability or queue limit)"};
                return R"({"accepted":true})";
            }
            if(method=="sounds.get")
            {
                fields(p,{"node"});const auto s=c.sounds(node(c,member(p,"node")));
                std::vector<std::string> contexts;for(const auto& v:s.contexts)contexts.push_back("{\"id\":"+std::to_string(v.id)+",\"name\":"+quote(v.name)+",\"selectedChannels\":"+std::to_string(v.selectedChannels)+",\"activeVoices\":"+std::to_string(v.activeVoices)+",\"idle\":"+(v.idle?"true":"false")+'}');
                return "{\"capacity\":"+std::to_string(s.capacity)+",\"mode\":"+quote(s.mode)+",\"contexts\":"+join(contexts)+'}';
            }
            if(method=="sounds.prepare")
            {
                fields(p,{"node","name","parameters","state","preset","sfz","library","directory"});const auto n=node(c,member(p,"node"));
                std::vector<oscairn_sound_parameter_t> patch;
                auto optional=[&](const char* name){return member(p,name)?std::string(string(member(p,name))):std::string();};
                if(member(p,"parameters"))for(auto v:array(member(p,"parameters"),128,0)){fields(v,{"id","value"});patch.push_back({integer(member(v,"id")),number(member(v,"value"))});}
                try{return "{\"context\":"+std::to_string(c.prepareSound(n,std::string(string(member(p,"name"))),patch,optional("state"),optional("preset"),optional("sfz"),optional("library"),optional("directory")))+'}';}
                catch(const std::runtime_error& e){throw Failure{-32005,e.what()};}
            }
            if(method=="assets.register")
            {
                fields(p,{"folder"});
                try{const auto f=c.registerAssetFolder(std::string(string(member(p,"folder"))));return "{\"library\":"+quote(f.library)+",\"directory\":"+quote(f.directory)+'}';}
                catch(const std::runtime_error& e){throw Failure{-32005,e.what()};}
            }
            if(method=="assets.import")
            {
                fields(p,{"file"});
                try{const auto r=c.importAsset(std::string(string(member(p,"file"))));return "{\"library\":"+quote(r.library)+",\"path\":"+quote(r.path)+",\"sha256\":"+quote(r.sha256)+'}';}
                catch(const std::runtime_error& e){throw Failure{-32005,e.what()};}
            }
            if(method=="assets.libraries")
            {
                fields(p,{});std::vector<std::string> libraries;
                try{for(const auto& l:c.assetLibraries())libraries.push_back("{\"id\":"+quote(l.id)+",\"name\":"+quote(l.name)+",\"path\":"+quote(l.path)+",\"files\":"+std::to_string(l.files)+'}');}
                catch(const std::runtime_error& e){throw Failure{-32005,e.what()};}
                return "{\"libraries\":"+join(libraries)+'}';
            }
            if(method=="sounds.select" || method=="sounds.discard")
            {
                if(method=="sounds.select")fields(p,{"node","context","channel"});else fields(p,{"node","context","cutTail"});
                const auto n=node(c,member(p,"node")),id=integer(member(p,"context"));
                const double channel=member(p,"channel")?number(member(p,"channel")):-1;
                if(channel < -1 || channel>15 || std::floor(channel)!=channel)invalid("Invalid sound channel");
                if(!(method=="sounds.select"?c.selectSound(n,id,static_cast<int16_t>(channel)):c.discardSound(n,id,member(p,"cutTail")?boolean(member(p,"cutTail")):false)))
                    throw Failure{-32005,"Context unknown, busy, or selection queue full"};
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
                edges(graph.topology.notes,"notes");
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
                    ",\"nodes\":" + join(nodes) + ",\"connections\":" + join(connections) + ",\"mappings\":" + join(mappings) +
                    ",\"parts\":" + [&]{ std::vector<std::string> v; for( const auto& x : graph.parts ) v.push_back(partJson(x)); return join(v); }() +
                    ",\"wires\":" + [&]{ std::vector<std::string> v; for( const auto& x : graph.wires ) v.push_back(wireJson(x)); return join(v); }() + scenesJson(graph) + '}';
            }
            if(method=="effects.create") {
                fields(p,{"name","source"});
                try{return "{\"node\":"+std::to_string(c.addEffectContext(member(p,"name")?std::string(string(member(p,"name"))):"",node(c,member(p,"source"))))+'}';}
                catch(const std::runtime_error& e){throw Failure{-32004,e.what()};}
            }
            if(method=="effects.resources") {
                fields(p,{"node"});const auto r=c.effectResources(node(c,member(p,"node")));
                if(!r)return "null";
                return "{\"sharedBytes\":"+std::to_string(r->shared_bytes)+",\"contextBytes\":"+std::to_string(r->context_bytes)+",\"capacity\":"+std::to_string(r->capacity)+",\"prepared\":"+std::to_string(r->prepared)+",\"tailActive\":"+(r->tail_active?"true":"false")+'}';
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
            if(method=="graph.replace")
            {
                fields(p,{"node","path","pluginId","preset"});
                const auto path=std::string(string(member(p,"path")));
                if(path.empty() || path.find('\0')!=std::string::npos)invalid("Invalid plugin path");
                try{c.replace(node(c,member(p,"node")),path,std::string(string(member(p,"pluginId"))),member(p,"preset")?std::string(string(member(p,"preset"))):std::string{});}
                catch(const std::runtime_error& e){throw Failure{-32004,e.what()};}
                return "{\"revision\":"+std::to_string(c.graphState().revision)+'}';
            }
            if( method == "pads.get" ) { fields(p,{}); return padsJson(c.padMap()); }
            if( method == "pads.set" ) { fields(p,{"map"}); c.setPadMap(padsFrom(member(p,"map"))); return R"({"accepted":true})"; }
            if( method == "pads.play" )
            {
                fields(p,{"pad","on","velocity","channel"});
                const auto pad = integer(member(p,"pad"),PadMap::pads - 1);
                const bool on = boolean(member(p,"on"));
                const double velocity = member(p,"velocity") ? number(member(p,"velocity")) : (on ? 1.0 : 0.0);
                if( velocity < 0 || velocity > 1 ) invalid("velocity is 0..1");
                if( !c.pad(pad,on,velocity,member(p,"channel") ? int(integer(member(p,"channel"),15)) : 0) ) throw Failure{ -32005, "UI note queue full; emergency release scheduled" };
                return R"({"accepted":true})";
            }
            if( method == "pads.pressure" )
            {
                fields(p,{"pad","pressure"});
                const auto pressure = number(member(p,"pressure"));
                if( pressure < 0 || pressure > 1 ) invalid("pressure is 0..1");
                if( !c.padPressure(integer(member(p,"pad"),PadMap::pads - 1),pressure) ) throw Failure{ -32005, "UI note queue full; emergency release scheduled" };
                return R"({"accepted":true})";
            }
            if( method == "graph.remove" )
            {
                fields(p,{"node","userWires"});
                auto choice = UserWires::Remove;
                if( member(p,"userWires") )
                {
                    const auto s = string(member(p,"userWires"));
                    if( s == "reconnect" ) choice = UserWires::Reconnect; else if( s != "remove" ) invalid("userWires is remove or reconnect");
                }
                c.remove(node(c,member(p,"node")),choice);
                return R"({"accepted":true})";
            }
            if( method.substr(0,9) == "controls." )
            {
                try
                {
                    if( method == "controls.add" ) { fields(p,{"name"}); return "{\"control\":" + std::to_string(c.addMacro(member(p,"name") ? std::string(string(member(p,"name"))) : std::string())) + '}'; }
                    if( method == "controls.remove" ) { fields(p,{"control"}); c.removeMacro(integer(member(p,"control"))); return R"({"accepted":true})"; }
                    if( method == "controls.name" ) { fields(p,{"control","name"}); c.setMacroName(integer(member(p,"control")),std::string(string(member(p,"name")))); return R"({"accepted":true})"; }
                    if( method == "controls.targets" ) { fields(p,{"control","targets"}); c.setMacroTargets(integer(member(p,"control")),targetsFrom(c,member(p,"targets"))); return R"({"accepted":true})"; }
                    if( method == "controls.set" ) { fields(p,{"control","value"}); const auto v = number(member(p,"value")); if( v < 0 || v > 1 ) invalid("value is 0..1"); c.setMacro(integer(member(p,"control")),v); return R"({"accepted":true})"; }
                }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
            }
            if( method.substr(0,7) == "scenes." )
            {
                try
                {
                    if( method == "scenes.add" ) { fields(p,{"name"}); return "{\"scene\":" + std::to_string(c.addScene(member(p,"name") ? std::string(string(member(p,"name"))) : std::string())) + '}'; }
                    if( method == "scenes.store" ) { fields(p,{"scene"}); c.storeScene(integer(member(p,"scene"))); return R"({"accepted":true})"; }
                    if( method == "scenes.select" ) { fields(p,{"scene"}); c.selectScene(integer(member(p,"scene"))); return R"({"accepted":true})"; }
                    if( method == "scenes.remove" ) { fields(p,{"scene"}); c.removeScene(integer(member(p,"scene"))); return R"({"accepted":true})"; }
                    if( method == "scenes.name" ) { fields(p,{"scene","name"}); c.setSceneName(integer(member(p,"scene")),std::string(string(member(p,"name")))); return R"({"accepted":true})"; }
                    if( method == "scenes.parameters" )
                    {
                        fields(p,{"parameters"});
                        const auto list = member(p,"parameters");
                        if( !list || list->type != json_type_array ) invalid("parameters is an array of {node, param}");
                        std::vector<SceneParameter> parameters;
                        for( auto* e = static_cast<const json_array_s*>(list->payload)->start; e; e = e->next )
                        {
                            fields(e->value,{"node","param"});
                            if( parameters.size() == 4096 ) invalid("At most 4096 scene parameters");
                            parameters.push_back({node(c,member(e->value,"node")),integer(member(e->value,"param"))});
                        }
                        c.setSceneParameters(parameters);
                        return R"({"accepted":true})";
                    }
                }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
            }
            if( method == "edit.undo" || method == "edit.redo" || method == "edit.history" )
            {
                if( method == "edit.undo" ) c.undo();
                if( method == "edit.redo" ) c.redo();
                const auto h = c.history();
                return "{\"undo\":" + quote(h.undo) + ",\"redo\":" + quote(h.redo) + '}';
            }
            if( method.substr(0,6) == "parts." || method.substr(0,6) == "wires." )
            {
                try
                {
                    if( method == "parts.add" )
                    {
                        fields(p,{"name","input","output"});
                        PartInput input;
                        if( const auto in = member(p,"input") ) input = inputFrom(in);
                        const auto name = member(p,"name") ? std::string(string(member(p,"name"))) : std::string();
                        const auto output = member(p,"output") ? std::optional<uint32_t>(integer(member(p,"output"))) : std::nullopt;
                        return "{\"part\":" + std::to_string(c.addPart(name,input,output)) + '}';
                    }
                    if( method == "parts.output" )
                    {
                        fields(p,{"part","bus","port"});
                        const auto bus = member(p,"bus") ? integer(member(p,"bus")) : 0;
                        if( bus && member(p,"port") ) invalid("A part plays through a bus or to a port, not both");
                        c.setPartOutput(integer(member(p,"part")),bus,member(p,"port") ? std::optional<uint32_t>(integer(member(p,"port"))) : std::nullopt);
                        return R"({"accepted":true})";
                    }
                    if( method == "parts.send" )
                    {
                        fields(p,{"part","bus","level"});
                        c.setSend(integer(member(p,"part")),integer(member(p,"bus")),member(p,"level") ? std::optional<double>(number(member(p,"level"))) : std::nullopt);
                        return R"({"accepted":true})";
                    }
                    if( method == "parts.level" ) { fields(p,{"part","level","mute"}); c.setPartLevel(integer(member(p,"part")),number(member(p,"level")),boolean(member(p,"mute"))); return R"({"accepted":true})"; }
                    if( method == "parts.pan" ) { fields(p,{"part","pan"}); c.setPartPan(integer(member(p,"part")),number(member(p,"pan"))); return R"({"accepted":true})"; }
                    if( method == "parts.solo" ) { fields(p,{"part","solo"}); c.setPartSolo(integer(member(p,"part")),boolean(member(p,"solo"))); return R"({"accepted":true})"; }
                    if( method == "parts.meters" )
                    {
                        std::vector<std::string> meters;
                        for( const auto& m : c.partMeters() ) meters.push_back("{\"part\":" + std::to_string(m.part) + ",\"peak\":" + encode(m.peak) + '}');
                        return "{\"meters\":" + join(meters) + '}';
                    }
                    if( method == "parts.name" ) { fields(p,{"part","name"}); c.setPartName(integer(member(p,"part")),std::string(string(member(p,"name")))); return R"({"accepted":true})"; }
                    if( method == "parts.input" ) { fields(p,{"part","input"}); c.setPartInput(integer(member(p,"part")),inputFrom(member(p,"input"))); return R"({"accepted":true})"; }
                    if( method == "wires.zones" ) { fields(p,{"wire","zones"}); c.setZones(integer(member(p,"wire")),zonesFrom(member(p,"zones"))); return R"({"accepted":true})"; }
                    if( method == "parts.remove" ) { fields(p,{"part"}); c.removePart(integer(member(p,"part"))); return R"({"accepted":true})"; }
                    if( method == "parts.insert" ) { fields(p,{"part","index","node","split","lane"}); c.insertIntoPart(integer(member(p,"part")),integer(member(p,"index")),node(c,member(p,"node")),placeFrom(p)); return R"({"accepted":true})"; }
                    if( method == "parts.removeNode" ) { fields(p,{"part","node"}); c.removeFromPart(integer(member(p,"part")),node(c,member(p,"node"))); return R"({"accepted":true})"; }
                    if( method == "parts.move" ) { fields(p,{"part","from","to","split","lane"}); c.movePartNode(integer(member(p,"part")),integer(member(p,"from")),integer(member(p,"to")),placeFrom(p)); return R"({"accepted":true})"; }
                    if( method == "parts.addSplit" )
                    {
                        fields(p,{"part","index","split","lane","kind","splitter","joiner"});
                        auto kind = Split::Kind::Copy;
                        if( member(p,"kind") )
                        {
                            const auto name = string(member(p,"kind"));
                            const auto at = std::find_if(splitKinds.begin(), splitKinds.end(), [&](const char* k) { return name == k; });
                            if( at == splitKinds.end() ) invalid("kind is copy, leftRight, midSide or bands");
                            kind = Split::Kind(at - splitKinds.begin());
                        }
                        const NodeId splitter = member(p,"splitter") ? node(c,member(p,"splitter")) : 0, joiner = member(p,"joiner") ? node(c,member(p,"joiner")) : 0;
                        return "{\"split\":" + std::to_string(c.addSplit(integer(member(p,"part")),integer(member(p,"index")),placeFrom(p),kind,splitter,joiner)) + '}';
                    }
                    if( method == "parts.removeSplit" ) { fields(p,{"part","split"}); c.removeSplit(integer(member(p,"part")),integer(member(p,"split"))); return R"({"accepted":true})"; }
                    if( method == "parts.addLane" ) { fields(p,{"part","split"}); return "{\"lane\":" + std::to_string(c.addLane(integer(member(p,"part")),integer(member(p,"split")))) + '}'; }
                    if( method == "parts.removeLane" ) { fields(p,{"part","split","lane"}); c.removeLane(integer(member(p,"part")),integer(member(p,"split")),integer(member(p,"lane"))); return R"({"accepted":true})"; }
                    if( method == "parts.lane" )
                    {
                        // Fields left out keep their current values.
                        fields(p,{"part","split","lane","zones","level","pan","mute","solo"});
                        const auto part = integer(member(p,"part")), split = integer(member(p,"split")), lane = integer(member(p,"lane"));
                        Lane l;
                        for( const auto& x : c.graphState().parts ) if( x.id == part )
                            for( const auto& s : x.splits ) if( s.id == split && lane < s.lanes.size() ) l = s.lanes[lane];
                        if( member(p,"zones") ) l.zones = zonesFrom(member(p,"zones"));
                        if( member(p,"level") ) l.level = number(member(p,"level"));
                        if( member(p,"pan") ) l.pan = number(member(p,"pan"));
                        if( member(p,"mute") ) l.mute = boolean(member(p,"mute"));
                        if( member(p,"solo") ) l.solo = boolean(member(p,"solo"));
                        c.setLane(part,split,lane,l);
                        return R"({"accepted":true})";
                    }
                    if( method == "wires.connect" )
                    {
                        fields(p,{"kind","from","to","fromPort","toPort","channels","level","pan","zones"});
                        Wire w;
                        const auto kind = string(member(p,"kind"));
                        if( kind != "audio" && kind != "notes" ) invalid("Unknown wire kind");
                        w.kind = kind == "audio" ? WireKind::Audio : WireKind::Notes;
                        w.from = node(c,member(p,"from"),true); w.to = node(c,member(p,"to"),true);
                        if( w.kind == WireKind::Audio ) { w.fromPort = integer(member(p,"fromPort")); w.toPort = integer(member(p,"toPort")); }
                        else if( member(p,"fromPort") || member(p,"toPort") || member(p,"channels") ) invalid("Note wires have no ports or channels");
                        if( const auto pairs = member(p,"channels") )
                        {
                            if( pairs->type != json_type_array ) invalid("channels is an array of [from, to] pairs");
                            for( auto* e = static_cast<const json_array_s*>(pairs->payload)->start; e; e = e->next )
                            {
                                if( !e->value || e->value->type != json_type_array || static_cast<const json_array_s*>(e->value->payload)->length != 2 ) invalid("channels is an array of [from, to] pairs");
                                const auto* first = static_cast<const json_array_s*>(e->value->payload)->start;
                                w.channels.push_back({integer(first->value,Graph::maxAudioChannels - 1),integer(first->next->value,Graph::maxAudioChannels - 1)});
                                if( w.channels.size() > Graph::maxAudioChannels ) invalid("Too many channel pairs");
                            }
                        }
                        if( member(p,"zones") ) { if( w.kind != WireKind::Notes ) invalid("Only note wires have zones"); w.zones = zonesFrom(member(p,"zones")); }
                        if( member(p,"level") ) w.level = number(member(p,"level"));
                        if( member(p,"pan") ) w.pan = number(member(p,"pan"));
                        return "{\"wire\":" + std::to_string(c.connectPorts(w)) + '}';
                    }
                    if( method == "wires.disconnect" ) { fields(p,{"wire"}); c.disconnectWire(integer(member(p,"wire"))); return R"({"accepted":true})"; }
                    if( method == "wires.mix" )
                    {
                        fields(p,{"wire","level","pan","mute","solo"});
                        c.setMix(integer(member(p,"wire")),number(member(p,"level")),number(member(p,"pan")),boolean(member(p,"mute")),boolean(member(p,"solo")));
                        return R"({"accepted":true})";
                    }
                }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
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
                    route.gain=member(p,"gain")?number(member(p,"gain")):1.0;
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

        void preflight(std::string_view text) {
            const auto error=detail::jsonPreflight(text,JsonRpc::maxDepth);
            if(!error.empty())throw Failure{error=="JSON nesting limit exceeded"?-32006:-32700,std::string(error)};
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
