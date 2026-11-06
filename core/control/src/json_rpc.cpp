// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/json_rpc.h"
#include "JsonInput.h"
#include "CurveJson.h"
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
        // A control group of controllers.set (RFC 0024's control groups, as the app resolves them).
        template<class Names> size_t named( const Names& names, std::string_view name, const char* message )
        {
            const auto at = std::find_if( names.begin(), names.end(), [&]( const char* n ) { return name == n; } );
            if( at == names.end() ) invalid( message );
            return size_t( at - names.begin() );
        }
        ControlGroup controlGroup( Value v )
        {
            fields( v, {"role","kind","count","rows","modes","functions","scenes","paged","switches","panic","midi"} );
            ControlGroup g;
            static constexpr std::array<const char*, 4> roles{"notes","screen","assign","fixed"};
            g.role = ControlGroup::Role( named( roles, string( member( v, "role" ) ), "a group's role is notes, screen, assign or fixed" ) );
            g.kind = std::string( string( member( v, "kind" ) ) );
            g.count = integer( member( v, "count" ), 512 );
            if( const auto rows = member( v, "rows" ) )
                for( auto r : array( rows, 64, 0 ) ) { fields( r, {"name","bank"} ); g.rows.push_back( {std::string( string( member( r, "name" ) ) ), BankKind( named( bankKinds, string( member( r, "bank" ) ), "unknown bank" ) )} ); }
            if( const auto modes = member( v, "modes" ) ) for( auto m : array( modes, 16, 0 ) ) g.modes.push_back( std::string( string( m ) ) );
            if( const auto functions = member( v, "functions" ) ) for( auto f : array( functions, 512, 0 ) ) g.functions.push_back( std::string( string( f ) ) );
            g.scenes = member( v, "scenes" ) && boolean( member( v, "scenes" ) );
            g.paged = member( v, "paged" ) && boolean( member( v, "paged" ) );
            g.switches = member( v, "switches" ) && boolean( member( v, "switches" ) );
            g.panic = member( v, "panic" ) && boolean( member( v, "panic" ) );
            if( const auto m = member( v, "midi" ) )
            {
                fields( m, {"device","port","channel","channels","message","numbers","encodings","steps","latch","feedback"} );
                auto& x = g.midi;
                x.device = std::string( string( member( m, "device" ) ) );
                if( member( m, "port" ) ) { x.port = integer( member( m, "port" ) ); if( !x.port ) invalid( "ports count from 1" ); }
                auto channel = [&]( Value c ) { const auto n = integer( c, 16 ); if( !n ) invalid( "channels are 1 to 16" ); return n; };
                if( member( m, "channel" ) ) x.channel = channel( member( m, "channel" ) );
                if( const auto list = member( m, "channels" ) ) for( auto c : array( list, 512, 0 ) ) x.channels.push_back( channel( c ) );
                static constexpr std::array<const char*, 4> messages{"cc","note","pitchBend","pressure"};
                if( member( m, "message" ) ) x.message = ControlGroup::Midi::Message( named( messages, string( member( m, "message" ) ), "a group's message is cc, note, pitchBend or pressure" ) );
                if( const auto numbers = member( m, "numbers" ) ) for( auto n : array( numbers, 4096, 0 ) ) x.numbers.push_back( uint8_t( integer( n, 127 ) ) );
                if( const auto list = member( m, "encodings" ) )
                    for( auto e : array( list, 512, 0 ) )
                        x.encodings.push_back( string( e ) == "steps" ? Encoding::Steps : Encoding( named( encodings, string( e ), "unknown encoding" ) ) );
                if( const auto list = member( m, "steps" ) )
                    for( auto s : array( list, 512, 0 ) ) { const auto pair = array( s, 2, 2 ); x.steps.push_back( {uint8_t( integer( pair[0], 127 ) ), uint8_t( integer( pair[1], 127 ) )} ); }
                x.latch = member( m, "latch" ) && boolean( member( m, "latch" ) );
                x.feedback = member( m, "feedback" ) && boolean( member( m, "feedback" ) );
            }
            return g;
        }
        // An export's optional sign: {key, items, archive} (RFC 0006).
        Control::ExportSigning exportSigning( Value v )
        {
            Control::ExportSigning s{};
            if( !v ) return s;
            fields( v, { "key", "items", "archive" } );
            s.key = std::string( string( detail::member( v, "key" ) ) );
            s.items = detail::member( v, "items" ) && boolean( detail::member( v, "items" ) );
            s.archive = detail::member( v, "archive" ) && boolean( detail::member( v, "archive" ) );
            return s;
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
        std::string devicesJson( const std::vector<DeviceRule>& rules );
        std::vector<DeviceRule> devicesFrom( Value v );
        std::string controllers( Control& c )
        {
            const auto s = c.controllerState();
            std::vector<std::string> positions;
            for( const auto p : s.positions ) positions.push_back( std::to_string( p ) );
            return "{\"positions\":" + join( positions ) + ",\"touched\":" + quote( s.touched ) + ",\"touches\":" + std::to_string( s.touches ) + '}';
        }
        std::string transport( Control& c )
        {
            const auto s = c.transportState();
            return std::string("{\"playing\":") + ( s.playing ? "true" : "false" ) + ",\"tempo\":" + encode(s.tempo) +
                ",\"beat\":" + encode(s.beat) + ",\"clockSource\":" +
                quote(c.clockSource() == Transport::ClockSource::Internal ? "internal" : "midi") + ",\"clockOutputs\":" + devicesJson(s.clockOutputs) + '}';
        }
        std::string takeJson(const Control::Take& t)
        {
            std::vector<std::string> files,overruns;
            for(const auto& f:t.files)files.push_back(quote(f));
            for(const auto& o:t.overruns)overruns.push_back("{\"start\":"+std::to_string(o.start)+",\"frames\":"+std::to_string(o.frames)+'}');
            return "{\"take\":"+std::to_string(t.take)+",\"state\":"+quote(t.state)+",\"folder\":"+quote(t.folder)+",\"files\":"+join(files)+
                ",\"start\":"+std::to_string(t.start)+",\"frames\":"+std::to_string(t.frames)+",\"sampleRate\":"+encode(t.sampleRate)+",\"overruns\":"+join(overruns)+
                ",\"droppedNotes\":"+std::to_string(t.droppedNotes)+",\"behind\":"+(t.behind?"true":"false")+",\"playing\":"+(t.playing?"true":"false")+",\"error\":"+quote(t.error)+'}';
        }
        std::string status(Control& c)
        {
            const auto s = c.sessionStatus();
            auto endpoints = [](const std::vector<MidiEndpoint>& list) {
                std::vector<std::string> out;
                for(const auto& e : list)
                    out.push_back("{\"source\":" + std::to_string(e.source) + ",\"name\":" + quote(e.name) + ",\"kind\":\"" + endpointKinds[size_t(e.kind)] + "\",\"port\":" + std::to_string(e.port) + '}');
                return join(out);
            };
            auto channels=[](const auto& list){std::vector<std::string> result;for(const auto& ch:list)result.push_back("{\"deviceChannel\":"+std::to_string(ch.deviceChannel)+",\"name\":"+quote(ch.name)+",\"available\":"+(ch.available?"true":"false")+'}');return join(result);};
            return "{\"audioReady\":"+std::string(s.audioReady?"true":"false")+",\"audioInputs\":"+channels(s.audioInputs)+",\"audioOutputs\":"+channels(s.audioOutputs)+",\"audioDevice\":" + quote(s.audioDevice) + ",\"midiInputs\":" + endpoints(s.midiInputs) + ",\"midiOutputs\":" + endpoints(s.midiOutputs) +
                ",\"sampleRate\":" + encode(s.sampleRate) + ",\"bufferSize\":" + std::to_string(s.bufferSize) +
                ",\"cpuLoad\":" + encode(s.cpuLoad) + ",\"deadlineMisses\":" + std::to_string(s.deadlineMisses) + ",\"graphEventDrops\":" + std::to_string(s.graphEventDrops) + ",\"midiEventDrops\":" + std::to_string(s.midiEventDrops) +
                ",\"failures\":" + [&]{ std::vector<std::string> f; for(const auto& x : s.failures) f.push_back("{\"node\":" + std::to_string(x.node) + ",\"name\":" + quote(x.name) + ",\"reason\":" + quote(x.reason) + '}'); return join(f); }() +
                ",\"notices\":" + [&]{ std::vector<std::string> n; for(const auto& x : s.notices) n.push_back(quote(x)); return join(n); }() +
                ",\"workers\":" + [&]{ std::vector<std::string> w; for(const auto& x : s.workers) w.push_back("{\"node\":" + std::to_string(x.node) + ",\"name\":" + quote(x.name) + ",\"cpu\":" + encode(x.cpu) + ",\"memory\":" + std::to_string(x.memory) + ",\"block\":" + encode(x.block) + ",\"blockMax\":" + encode(x.blockMax) + '}'); return join(w); }() + '}';
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
            fields(in,{"kind","port","zones","devices","maxNotes"});
            PartInput input;
            const auto kind = string(member(in,"kind"));
            input.kind = kind == "notes" ? PartInput::Kind::Notes : kind == "audio" ? PartInput::Kind::Audio : kind == "none" ? PartInput::Kind::None : kind == "bus" ? PartInput::Kind::Bus : (invalid("input kind is notes, audio, none or bus"), PartInput::Kind::None);
            input.port = member(in,"port") ? integer(member(in,"port")) : 0;
            if( member(in,"zones") ) input.zones = zonesFrom(member(in,"zones"));
            if( member(in,"devices") ) input.devices = devicesFrom(member(in,"devices"));
            if( member(in,"maxNotes") ) input.maxNotes = uint16_t(integer(member(in,"maxNotes"),128));
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
        const char* kindName( MacroTarget::Kind k ) { return k == MacroTarget::Kind::Parameter ? "parameter" : k == MacroTarget::Kind::PartLevel ? "partLevel" : k == MacroTarget::Kind::PartMute ? "partMute" : k == MacroTarget::Kind::Morph ? "morph" : "control"; }
        std::string macrosJson( const std::vector<Macro>& macros )
        {
            std::vector<std::string> out;
            for( const auto& m : macros )
            {
                std::vector<std::string> targets;
                for( const auto& t : m.targets )
                    targets.push_back(std::string("{\"kind\":\"") + kindName(t.kind) + "\",\"node\":" + std::to_string(t.node) + ",\"param\":" + std::to_string(t.param) +
                        ",\"part\":" + std::to_string(t.part) + ",\"control\":" + std::to_string(t.macro) + ",\"from\":" + std::to_string(t.sceneFrom) + ",\"to\":" + std::to_string(t.sceneTo) + ",\"start\":" + encode(t.start) + ",\"end\":" + encode(t.end) +
                        ",\"curve\":" + detail::curveJson(t, encode) + ",\"relative\":" + (t.relative ? "true" : "false") + ",\"base\":" + encode(t.base) + ",\"offIn\":" + [&] {
                            std::vector<std::string> ids; for( const auto id : t.offIn ) ids.push_back(std::to_string(id)); return join(ids); }() + '}');
                out.push_back("{\"id\":" + std::to_string(m.id) + ",\"part\":" + std::to_string(m.part) + ",\"name\":" + quote(m.name) + ",\"value\":" + encode(m.value) + ",\"targets\":" + join(targets) + '}');
            }
            return join(out);
        }
        std::vector<MacroTarget> targetsFrom( Control& c, Value list )
        {
            if( !list || list->type != json_type_array ) invalid("targets is an array");
            std::vector<MacroTarget> targets;
            for( auto* e = static_cast<const json_array_s*>(list->payload)->start; e; e = e->next )
            {
                fields(e->value,{"kind","node","param","part","control","from","to","start","end","curve","relative","base","offIn"});
                MacroTarget t;
                const auto kind = string(member(e->value,"kind"));
                t.kind = kind == "parameter" ? MacroTarget::Kind::Parameter : kind == "partLevel" ? MacroTarget::Kind::PartLevel : kind == "partMute" ? MacroTarget::Kind::PartMute : kind == "control" ? MacroTarget::Kind::Macro : kind == "morph" ? MacroTarget::Kind::Morph : (invalid("kind is parameter, partLevel, partMute, control or morph"), MacroTarget::Kind::Parameter);
                if( t.kind == MacroTarget::Kind::Parameter ) { t.node = node(c,member(e->value,"node")); t.param = integer(member(e->value,"param")); }
                if( t.kind == MacroTarget::Kind::PartLevel || t.kind == MacroTarget::Kind::PartMute ) t.part = integer(member(e->value,"part"));
                if( t.kind == MacroTarget::Kind::Macro ) t.macro = integer(member(e->value,"control"));
                if( t.kind == MacroTarget::Kind::Morph ) { t.sceneFrom = integer(member(e->value,"from")); t.sceneTo = integer(member(e->value,"to")); }
                if( member(e->value,"start") ) t.start = number(member(e->value,"start"));
                if( member(e->value,"end") ) t.end = number(member(e->value,"end"));
                if( const auto curve = member(e->value,"curve"); curve && !detail::curveFrom(curve, t) )
                    invalid("curve is linear, exponential, logarithmic, s-curve, {kind: db, min, max} (-144 <= min < max <= 0) or {kind: power, exponent} (0.1 to 10)");
                if( member(e->value,"relative") ) t.relative = boolean(member(e->value,"relative"));
                if( const auto off = member(e->value,"offIn") )
                {
                    if( off->type != json_type_array ) invalid("offIn is an array of scene IDs");
                    for( auto* x = static_cast<const json_array_s*>(off->payload)->start; x; x = x->next ) { if( t.offIn.size() == 256 ) invalid("At most 256 scenes"); t.offIn.push_back(integer(x->value)); }
                }
                targets.push_back(t);
            }
            return targets;
        }
        std::string wireRefJson( const WireRef& r )
        {
            return std::string("\"kind\":") + (r.kind == WireKind::Audio ? "\"audio\"" : "\"notes\"") + ",\"part\":" + std::to_string(r.part) + ",\"from\":" + std::to_string(r.from) +
                ",\"to\":" + std::to_string(r.to) + ",\"fromPort\":" + std::to_string(r.fromPort) + ",\"toPort\":" + std::to_string(r.toPort) + ",\"send\":" + std::to_string(r.send) +
                ",\"split\":" + std::to_string(r.split) + ",\"lane\":" + std::to_string(r.lane) + ",\"fader\":" + (r.fader ? "true" : "false");
        }
        std::string scenesJson( const Control::GraphState& g )
        {
            std::vector<std::string> scenes, parameters;
            auto flag = [](bool b) { return b ? "true" : "false"; };
            for( const auto& s : g.scenes )
            {
                std::vector<std::string> scope, off, offWires, parts, mixes, values, macros;
                for( size_t k = 0; k < sceneKinds.size(); ++k ) if( s.scope & (1u << k) ) scope.push_back(quote(sceneKinds[k]));
                for( const auto n : s.off ) off.push_back(std::to_string(n));
                for( const auto& r : s.offWires ) offWires.push_back('{' + wireRefJson(r) + '}');
                for( const auto& p : s.parts )
                {
                    std::vector<std::string> sends, lanes;
                    for( const auto& x : p.sends ) sends.push_back("{\"bus\":" + std::to_string(x.bus) + ",\"level\":" + encode(x.level) + '}');
                    for( const auto& l : p.lanes ) lanes.push_back("{\"split\":" + std::to_string(l.split) + ",\"lane\":" + std::to_string(l.lane) + ",\"level\":" + encode(l.level) +
                        ",\"pan\":" + encode(l.pan) + ",\"mute\":" + flag(l.mute) + ",\"solo\":" + flag(l.solo) + '}');
                    parts.push_back("{\"part\":" + std::to_string(p.part) + ",\"level\":" + encode(p.level) + ",\"pan\":" + encode(p.pan) + ",\"mute\":" + flag(p.mute) +
                        ",\"solo\":" + flag(p.solo) + ",\"sends\":" + join(sends) + ",\"lanes\":" + join(lanes) + '}');
                }
                for( const auto& m : s.wires ) mixes.push_back('{' + wireRefJson(m.wire) + ",\"level\":" + encode(m.level) + ",\"pan\":" + encode(m.pan) + ",\"mute\":" + flag(m.mute) + ",\"solo\":" + flag(m.solo) + '}');
                for( const auto& v : s.values ) values.push_back("{\"node\":" + std::to_string(v.at.node) + ",\"param\":" + std::to_string(v.at.param) + ",\"value\":" + encode(v.value) + '}');
                for( const auto& [m, v] : s.macros ) macros.push_back("{\"control\":" + std::to_string(m) + ",\"value\":" + encode(v) + '}');
                scenes.push_back("{\"id\":" + std::to_string(s.id) + ",\"name\":" + quote(s.name) + ",\"scope\":" + join(scope) +
                    ",\"glide\":" + std::to_string(s.glide) + ",\"curve\":" + quote(sceneCurves[size_t(s.curve)]) + ",\"launch\":" + quote(sceneLaunches[size_t(s.launch)]) + ",\"off\":" + join(off) + ",\"offWires\":" + join(offWires) +
                    ",\"parts\":" + join(parts) + ",\"wires\":" + join(mixes) + ",\"values\":" + join(values) + ",\"controls\":" + join(macros) +
                    ",\"master\":{\"level\":" + encode(s.masterLevel) + ",\"pan\":" + encode(s.masterPan) + "}}");
            }
            for( const auto& at : g.untouched ) parameters.push_back("{\"node\":" + std::to_string(at.node) + ",\"param\":" + std::to_string(at.param) + '}');
            return ",\"scenes\":" + join(scenes) + ",\"sceneUntouched\":" + join(parameters) + ",\"scene\":" + std::to_string(g.scene) + ",\"controls\":" + macrosJson(g.macros) +
                ",\"master\":{\"level\":" + encode(g.masterLevel) + ",\"pan\":" + encode(g.masterPan) + '}';
        }
        std::string wireJson( const Wire& w )
        {
            std::vector<std::string> pairs;
            for( const auto& [a,b] : w.channels ) pairs.push_back("[" + std::to_string(a) + "," + std::to_string(b) + "]");
            return "{\"id\":" + std::to_string(w.id) + ",\"kind\":" + (w.kind == WireKind::Audio ? "\"audio\"" : "\"notes\"") +
                ",\"owner\":" + (w.owner == WireOwner::Part ? "\"part\",\"part\":" + std::to_string(w.part) : std::string("\"user\"")) +
                ",\"from\":" + std::to_string(w.from) + ",\"to\":" + std::to_string(w.to) + ",\"fromPort\":" + std::to_string(w.fromPort) +
                ",\"toPort\":" + std::to_string(w.toPort) + ",\"channels\":" + join(pairs) + ",\"level\":" + encode(w.level) + ",\"pan\":" + encode(w.pan) +
                ",\"mute\":" + (w.mute ? "true" : "false") + ",\"solo\":" + (w.solo ? "true" : "false") + ",\"zones\":" + zonesJson(w.zones) + ",\"send\":" + std::to_string(w.send) + ",\"split\":" + std::to_string(w.split) + ",\"lane\":" + std::to_string(w.lane) + ",\"fader\":" + (w.fader ? "true" : "false") + ",\"connected\":" + (w.connected ? "true" : "false") + ",\"enabled\":" + (w.enabled ? "true" : "false") + '}';
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
        // RFC 0033: a key's root and its scale's pitch classes above the root, with the scale's
        // name when it is one of the pads' scales (padScales), else null.
        std::string keyJson( const MusicalKey& k )
        {
            std::vector<int> steps; std::vector<std::string> text;
            for( int i = 0; i < 12; ++i ) if( k.scale >> i & 1 ) { steps.push_back(i); text.push_back(std::to_string(i)); }
            std::string name;
            for( const auto& [n, s] : padScales() ) if( s == steps ) name = n;
            return "{\"root\":" + std::to_string(k.root) + ",\"scale\":" + join(text) + ",\"name\":" + (name.empty() ? std::string("null") : quote(name)) + '}';
        }
        // A scale is a name from padScales, or pitch classes above the root (0 included).
        MusicalKey keyFrom( Value v )
        {
            fields(v,{"root","scale"});
            MusicalKey k{uint8_t(integer(member(v,"root"),11)),0};
            const auto scale = member(v,"scale");
            if( scale && scale->type == json_type_string )
            {
                const auto name = string(scale);
                const auto& scales = padScales();
                const auto found = std::find_if(scales.begin(),scales.end(),[&](const auto& s){ return s.first == name; });
                if( found == scales.end() ) invalid("Unknown scale");
                for( int i : found->second ) k.scale |= uint16_t(1u << i);
            }
            else for( auto s : array(scale,12) ) k.scale |= uint16_t(1u << integer(s,11));
            if( !k.valid() ) invalid("A scale holds its root, 0");
            return k;
        }

        std::string partJson( const Part& p )
        {
            const auto chain = chainJson(p.chain);
            return "{\"id\":" + std::to_string(p.id) + ",\"name\":" + quote(p.name) + ",\"input\":{\"kind\":\"" + inputKind(p.input.kind) +
                "\",\"port\":" + std::to_string(p.input.port) + ",\"zones\":" + zonesJson(p.input.zones) + ",\"devices\":" + devicesJson(p.input.devices) + ",\"maxNotes\":" + std::to_string(p.input.maxNotes) + "},\"chain\":" + chain + ",\"splits\":" + splitsJson(p.splits) + ",\"output\":" + std::to_string(p.output) +
                ",\"level\":" + encode(p.level) + ",\"pan\":" + encode(p.pan) + ",\"mute\":" + (p.mute ? "true" : "false") + ",\"solo\":" + (p.solo ? "true" : "false") + ",\"bus\":" + std::to_string(p.bus) + ",\"sends\":" + [&]{ std::vector<std::string> v; for( const auto& s : p.sends ) v.push_back("{\"bus\":" + std::to_string(s.bus) + ",\"level\":" + encode(s.level) + '}'); return join(v); }()
                + ",\"midiOut\":{\"devices\":" + devicesJson(p.midiOut.devices) + ",\"channel\":" + std::to_string(p.midiOut.channel) + '}'
                + ",\"key\":" + (p.key ? keyJson(*p.key) : std::string("null"))
                + ",\"from\":" + (p.from.sound.empty() ? std::string("null") : "{\"sound\":" + quote(p.from.sound) + ",\"revision\":" + quote(p.from.revision) + ",\"name\":" + quote(p.from.name) + ",\"modified\":" + (p.from.modified ? "true" : "false") + '}') + '}';
        }

        std::string invoke( Control& c, JsonRpc& client, std::string_view method, Value p )
        {
            if( method == "session.hello" )
            {
                fields(p, {});
                return R"({"protocolVersion":1,"server":"oscairn","capabilities":["performance.save","performance.load","performance.prepare","performance.export","budget.set","budget.estimate","performance.info","performance.setInfo","library.query","library.favorite","library.tag","library.edit","library.copy","library.export","audition.start","audition.stop","audition.get","record.start","record.stop","record.get","record.takes","library.rescan","library.contents","library.install","library.libraries","library.uninstall","library.delete","library.save","library.load","parts.saveSound","parts.loadSound","parts.fromSound","performance.contents","performance.import","state.info","state.save","window.info","window.show","presets.list","properties.list","properties.get","properties.set","properties.request","sounds.get","sounds.prepare","sounds.select","sounds.discard","sounds.presets","assets.register","assets.import","assets.libraries","session.status","engine.attach","engine.release","params.list","params.get","params.set","params.text","params.gesture","graph.audio-routing","graph.get","graph.add","effects.create","effects.resources","graph.remove","graph.replace","graph.connect","graph.disconnect","graph.map","graph.unmap","graph.commit","parts.add","parts.remove","parts.insert","parts.removeNode","parts.move","parts.addSplit","parts.removeSplit","parts.addLane","parts.removeLane","parts.lane","parts.input","parts.output","parts.send","parts.level","parts.name","parts.solo","parts.pan","master.set","graph.role","parts.meters","wires.meters","edit.undo","edit.redo","edit.history","wires.connect","wires.disconnect","wires.enable","graph.enable","wires.mix","wires.zones","scenes.add","scenes.store","scenes.select","scenes.remove","scenes.name","scenes.scope","scenes.transition","scenes.untouched","controls.add","controls.remove","controls.name","controls.targets","controls.set","subscribe","unsubscribe","transport.get","transport.set","notes.send","notes.panic","midi.ignore","controllers.set","controllers.view","controllers.get","pads.get","pads.set","pads.play","pads.pressure","assignments.get","assignments.set","parts.midiOut","trust.list","trust.set","trust.remove","keys.list","keys.make","record.retro","record.delete","record.rename","record.play","harmony.get","harmony.set"],"limits":{"messageBytes":65536,"depth":32,"batch":64,"parameterChanges":128}})";
            }
            if( method == "engine.attach" || method == "engine.release" )
            {
                fields(p,{});
                if( method == "engine.attach" ) client.attach(); else client.release();
                return R"({"accepted":true})";
            }
            if( method == "subscribe" || method == "unsubscribe" )
            {
                fields(p,{"topics","allChanges"});
                const auto topics = array(member(p,"topics"),7);
                const bool all = member(p,"allChanges") ? boolean(member(p,"allChanges")) : false;
                for( auto topic : topics )
                    if( string(topic) != "params.changed" && string(topic) != "transport" && string(topic) != "graph.changed" && string(topic) != "status" && string(topic) != "record" && string(topic) != "controllers" && string(topic) != "controls.changed" )
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
            if(method=="performance.prepare") {
                fields(p,{"directory"});
                try{return c.preparePerformance(std::string(string(member(p,"directory"))))?R"({"prepared":true})":R"({"prepared":false})";}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
            }
            // Resource budgets (RFC 0025).
            if(method=="budget.set") {
                fields(p,{"cpu","memory","device","system","floor","margin"});
                Control::Budget b;b.cpu=number(member(p,"cpu"));if(member(p,"memory"))b.memory=uint64_t(integer(member(p,"memory")));
                for(auto [key,to]:{std::pair{"device",&b.device},{"system",&b.system},{"floor",&b.floor},{"margin",&b.margin}})if(member(p,key))*to=uint64_t(integer(member(p,key)));
                try{c.setBudget(b);}catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                return R"({"accepted":true})";
            }
            // Memory classes (RFC 0025): apps' reservations and launches.
            auto appMemory=[&](Value v){fields(v,{"app","reserve","dropNext"});return Control::AppMemory{std::string(string(member(v,"app"))),uint64_t(integer(member(v,"reserve")))};};
            auto appList=[&](const std::vector<Control::AppMemory>& apps){std::vector<std::string> out;for(const auto& a:apps)out.push_back("{\"app\":"+quote(a.app)+",\"reserve\":"+std::to_string(a.reserve)+'}');return join(out);};
            if(method=="memory.alwaysAvailable") {
                fields(p,{"apps"});std::vector<Control::AppMemory> apps;for(auto a:array(member(p,"apps"),64))apps.push_back(appMemory(a));
                try{c.setAlwaysAvailable(apps);}catch(const std::invalid_argument& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="memory.launch") {
                const auto app=appMemory(p);Control::Launch l;
                try{l=c.launchApp(app,member(p,"dropNext") && boolean(member(p,"dropNext")));}catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                return std::string("{\"admitted\":")+(l.admitted?"true":"false")+",\"offerNext\":"+(l.offerNext?"true":"false")+",\"reason\":"+quote(l.reason)+'}';
            }
            if(method=="memory.closed") {fields(p,{"app"});c.appClosed(std::string(string(member(p,"app"))));return R"({"closed":true})";}
            if(method=="memory.state") {
                fields(p,{});const auto m=c.memoryState();
                return "{\"device\":"+std::to_string(m.device)+",\"reserved\":"+std::to_string(m.reserved)+",\"share\":"+std::to_string(m.share)+",\"floor\":"+std::to_string(m.floor)+
                    ",\"margin\":"+std::to_string(m.margin)+",\"committed\":"+std::to_string(m.committed)+",\"opportunistic\":"+std::to_string(m.opportunistic)+
                    ",\"running\":"+appList(m.running)+",\"alwaysAvailable\":"+appList(m.alwaysAvailable)+",\"nextPrepared\":"+(m.nextPrepared?"true":"false")+",\"nextCommitted\":"+(m.nextCommitted?"true":"false")+'}';
            }
            if(method=="budget.estimate") {
                fields(p,{"directory"});
                Control::Estimate e;
                try{e=c.estimate(member(p,"directory")?std::string(string(member(p,"directory"))):std::string());}catch(const std::exception& x){throw Failure{-32005,x.what()};}
                std::vector<std::string> plugins;
                for(const auto& x:e.plugins)plugins.push_back("{\"name\":"+quote(x.name)+",\"plugin\":"+quote(x.plugin)+",\"cpu\":"+encode(x.cpu)+",\"memory\":"+std::to_string(x.memory)+",\"measured\":"+(x.measured?"true":"false")+'}');
                return "{\"cpu\":"+encode(e.cpu)+",\"memory\":"+std::to_string(e.memory)+",\"fits\":"+(e.fits?"true":"false")+",\"seamless\":"+(e.seamless?"true":"false")+
                    ",\"budget\":{\"cpu\":"+encode(e.budget.cpu)+",\"memory\":"+std::to_string(e.budget.memory)+"},\"plugins\":"+join(plugins)+'}';
            }
            if(method=="performance.export") {
                fields(p,{"file","editedSounds","sign"});
                std::vector<std::string> notes;
                try{for(const auto& n:c.exportPerformance(std::string(string(member(p,"file"))),member(p,"editedSounds") && boolean(member(p,"editedSounds")),exportSigning(member(p,"sign"))))notes.push_back(quote(n));}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return "{\"exported\":true,\"notes\":"+join(notes)+'}';
            }
            if(method=="audition.start") {
                fields(p,{"id","part"});
                try{c.auditionStart(std::string(string(member(p,"id"))),member(p,"part")?integer(member(p,"part")):0);}
                catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="audition.stop") {
                fields(p,{"keep"});
                try{c.auditionStop(member(p,"keep") && boolean(member(p,"keep")));}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="audition.get") {
                fields(p,{});const auto a=c.audition();
                return "{\"id\":"+quote(a.id)+",\"kind\":"+quote(a.kind)+",\"part\":"+std::to_string(a.part)+'}';
            }
            // RFC 0020: recording.
            if(method=="record.start" || method=="record.retro") {
                if(method=="record.start")fields(p,{"taps","name","from","until","keepGoing"});else fields(p,{"seconds","taps"});
                std::vector<Control::RecordTap> taps;
                if(member(p,"taps"))for(auto v:array(member(p,"taps"),256,0)){
                    fields(v,{"kind","port","part","node","fader"});
                    Control::RecordTap t;const auto kind=string(member(v,"kind"));
                    using K=Control::RecordTap::Kind;
                    if(kind=="output")t.kind=K::Output;else if(kind=="input")t.kind=K::Input;else if(kind=="part")t.kind=K::Part;
                    else if(kind=="node")t.kind=K::Node;else if(kind=="notes")t.kind=K::Notes;else invalid("Unknown tap kind");
                    if(member(v,"port"))t.port=integer(member(v,"port"));
                    if(member(v,"part"))t.part=integer(member(v,"part"));
                    if(member(v,"node"))t.node=node(c,member(v,"node"));
                    if(member(v,"fader")){const auto f=string(member(v,"fader"));if(f!="post" && f!="pre")invalid("fader is post or pre");t.preFader=f=="pre";}
                    taps.push_back(t);
                }
                const auto refused=[](const std::exception& e,const char* reason){return Failure{-32007,e.what(),std::string("{\"reason\":\"")+reason+"\"}"};};
                if(method=="record.retro"){
                    const auto seconds=number(member(p,"seconds"));
                    if(!(seconds>=0 && seconds<=3600))invalid("seconds is 0 to 3600");
                    try{c.recordRetro(seconds,taps);}
                    catch(const std::length_error& e){throw refused(e,"memory");}
                    catch(const std::invalid_argument& e){throw refused(e,"unknownTap");}
                    catch(const std::logic_error& e){throw refused(e,"busy");}
                    return R"({"accepted":true})";
                }
                // When (RFC 0020 step 3): from now, seconds ago or a bar line; for a length or until stopped.
                Control::RecordWhen when;using When=Control::RecordWhen;
                if(const auto f=member(p,"from")){
                    if(f->type==json_type_string){if(string(f)!="now")invalid(R"(from is "now", {"secondsAgo":s} or {"bar":"next"|"last"})");}
                    else{
                        fields(f,{"secondsAgo","bar"});
                        if(member(f,"secondsAgo")){when.from=When::From::SecondsAgo;when.seconds=number(member(f,"secondsAgo"));if(!(when.seconds>=0 && when.seconds<=3600))invalid("secondsAgo is 0 to 3600");}
                        else if(member(f,"bar")){const auto b=string(member(f,"bar"));if(b!="next" && b!="last")invalid("bar is next or last");when.from=b=="next"?When::From::NextBar:When::From::LastBar;}
                        else invalid(R"(from is "now", {"secondsAgo":s} or {"bar":"next"|"last"})");
                    }
                }
                if(const auto u=member(p,"until")){
                    if(u->type==json_type_string){if(string(u)!="stop")invalid(R"(until is "stop", {"bars":n} or {"seconds":s})");}
                    else{
                        fields(u,{"bars","seconds"});
                        if(member(u,"bars")){when.until=When::Until::Bars;when.length=number(member(u,"bars"));}
                        else if(member(u,"seconds")){when.until=When::Until::Seconds;when.length=number(member(u,"seconds"));}
                        else invalid(R"(until is "stop", {"bars":n} or {"seconds":s})");
                        if(!(when.length>0 && when.length<=86400))invalid("a take's length is more than 0 and at most a day");
                    }
                }
                if(member(p,"keepGoing"))when.keepGoing=boolean(member(p,"keepGoing"));
                try{return "{\"take\":"+std::to_string(c.recordStart(taps,member(p,"name")?std::string(string(member(p,"name"))):std::string(),when))+'}';}
                catch(const std::out_of_range& e){throw refused(e,"retro");}
                catch(const std::invalid_argument& e){throw refused(e,"unknownTap");}
                catch(const std::logic_error& e){throw refused(e,"busy");}
                catch(const std::exception& e){throw refused(e,"drive");}
            }
            if(method=="record.stop") {
                fields(p,{"take"});
                try{c.recordStop(integer(member(p,"take")));}catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="record.get" || method=="record.takes") {
                fields(p,{});
                std::vector<std::string> takes;uint32_t recording=0;
                for(const auto& t:c.recordTakes()){
                    if(t.state=="recording")recording=t.take;
                    takes.push_back(takeJson(t));
                }
                return method=="record.get"?"{\"recording\":"+std::to_string(recording)+",\"retro\":{\"seconds\":"+encode(c.recordRetroSeconds())+"},\"takes\":"+join(takes)+'}':"{\"takes\":"+join(takes)+'}';
            }
            if(method=="record.delete" || method=="record.rename" || method=="record.play") {
                if(method=="record.rename")fields(p,{"take","name"});else fields(p,{"take"});
                const auto take=integer(member(p,"take"));
                try{
                    if(method=="record.delete")c.recordDelete(take);
                    else if(method=="record.rename")c.recordRename(take,std::string(string(member(p,"name"))));
                    else c.recordPlay(take);
                }
                catch(const std::length_error& e){throw Failure{-32007,e.what(),R"({"reason":"memory"})"};}
                catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::logic_error& e){throw Failure{-32007,e.what(),R"({"reason":"busy"})"};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="library.export") {
                fields(p,{"id","file","editedSounds","sign"});
                std::vector<std::string> notes;
                try{for(const auto& n:c.exportLibraryItem(std::string(string(member(p,"id"))),std::string(string(member(p,"file"))),member(p,"editedSounds") && boolean(member(p,"editedSounds")),exportSigning(member(p,"sign"))))notes.push_back(quote(n));}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return "{\"exported\":true,\"notes\":"+join(notes)+'}';
            }
            // RFC 0021: the open Performance's info and the library of saved ones.
            if(method=="performance.info" || method=="performance.setInfo") {
                auto tags=[&](const std::vector<std::string>& v){std::vector<std::string> out;for(const auto& t:v)out.push_back(quote(t));return join(out);};
                if(method=="performance.setInfo") {
                    fields(p,{"name","description","tags","color"});Control::PerformanceInfo info;
                    info.name=std::string(string(member(p,"name")));info.description=std::string(string(member(p,"description")));
                    for(auto t:array(member(p,"tags"),32,0))info.tags.push_back(std::string(string(t)));
                    if(member(p,"color"))info.color=std::string(string(member(p,"color")));
                    try{c.setPerformanceInfo(info);}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                } else fields(p,{});
                const auto i=c.performanceInfo();
                return "{\"id\":"+quote(i.id)+",\"name\":"+quote(i.name)+",\"description\":"+quote(i.description)+",\"tags\":"+tags(i.tags)+",\"color\":"+quote(i.color)+'}';
            }
            if(method=="library.query") {
                fields(p,{"text","tags","favorites","recent","limit","kind","source","id"});Control::LibraryQuery q;
                if(member(p,"id")){q.id=std::string(string(member(p,"id")));if(q.id.empty())invalid("Empty library ID");}
                if(member(p,"source")){q.source=std::string(string(member(p,"source")));if(q.source.empty())invalid("Empty library source");}
                if(member(p,"kind")){q.kind=std::string(string(member(p,"kind")));if(q.kind!="performance" && q.kind!="sound")invalid("Unknown library kind");}
                if(member(p,"text"))q.text=std::string(string(member(p,"text")));
                if(member(p,"tags"))for(auto t:array(member(p,"tags"),32,0))q.tags.push_back(std::string(string(t)));
                if(member(p,"favorites"))q.favorites=boolean(member(p,"favorites"));
                if(member(p,"recent"))q.recent=boolean(member(p,"recent"));
                if(member(p,"limit")){q.limit=integer(member(p,"limit"));if(!q.limit || q.limit>1000)invalid("Invalid limit");}
                Control::LibraryResult r;
                try{r=c.libraryQuery(q);}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                std::vector<std::string> items,counts;
                for(const auto& i:r.items){std::vector<std::string> t;for(const auto& x:i.tags)t.push_back(quote(x));
                    items.push_back("{\"id\":"+quote(i.id)+",\"kind\":"+quote(i.kind)+",\"name\":"+quote(i.name)+",\"description\":"+quote(i.description)+",\"path\":"+quote(i.path)+",\"tags\":"+join(t)+",\"favorite\":"+(i.favorite?"true":"false")+",\"recent\":"+std::to_string(i.recent)+",\"color\":"+quote(i.color)+",\"revision\":"+quote(i.revision)+",\"hash\":"+quote(i.hash)+",\"library\":"+quote(i.library)+'}');}
                for(const auto& [tag,n]:r.tags)counts.push_back("{\"tag\":"+quote(tag)+",\"count\":"+std::to_string(n)+'}');
                std::vector<std::string> colors,hidden;
                for(const auto& [tag,color]:r.tagColors)colors.push_back("{\"tag\":"+quote(tag)+",\"color\":"+quote(color)+'}');
                for(const auto& tag:r.hiddenTags)hidden.push_back(quote(tag));
                return "{\"items\":"+join(items)+",\"tags\":"+join(counts)+",\"total\":"+std::to_string(r.total)+",\"tagColors\":"+join(colors)+",\"hiddenTags\":"+join(hidden)+'}';
            }
            // Libraries from archives (RFC 0021).
            if(method=="library.contents" || method=="library.install") {
                const bool install=method=="library.install";
                if(install)fields(p,{"file","asLibrary","items","acceptUnsignedCode"});else fields(p,{"file"});
                const auto file=std::string(string(member(p,"file")));
                std::vector<std::string> chosen;
                if(install && member(p,"items"))for(auto t:array(member(p,"items"),65536,0))chosen.push_back(std::string(string(t)));
                auto signer=[](const Control::Signer& s){return "{\"kind\":"+quote(s.kind)+",\"name\":"+quote(s.name)+",\"version\":"+quote(s.version)+",\"platform\":"+quote(s.platform)+
                                                              ",\"status\":"+quote(s.status)+",\"key\":"+quote(s.key)+",\"keyName\":"+quote(s.keyName)+'}';};
                auto code=[&](const std::vector<Control::Signer>& list){std::vector<std::string> out;for(const auto& s:list)out.push_back(signer(s));return join(out);};
                try{
                    if(!install){
                        const auto a=c.archiveContents(file);std::vector<std::string> items;
                        for(const auto& i:a.items)items.push_back("{\"kind\":"+quote(i.kind)+",\"id\":"+quote(i.id)+",\"name\":"+quote(i.name)+'}');
                        return "{\"id\":"+quote(a.id)+",\"name\":"+quote(a.name)+",\"kind\":"+quote(a.kind)+",\"items\":"+join(items)+",\"code\":"+code(a.code)+",\"signer\":"+signer(a.signer)+'}';
                    }
                    const auto r=c.installArchive(file,member(p,"asLibrary") && boolean(member(p,"asLibrary")),chosen,member(p,"acceptUnsignedCode") && boolean(member(p,"acceptUnsignedCode")));
                    std::vector<std::string> notes;
                    for(const auto& n:r.notes)notes.push_back(quote(n));
                    return "{\"added\":"+std::to_string(r.added)+",\"skipped\":"+std::to_string(r.skipped)+",\"notes\":"+join(notes)+",\"code\":"+code(r.code)+'}';
                }catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
            }
            if(method=="library.libraries") {
                fields(p,{});std::vector<std::string> out;
                for(const auto& l:c.libraries())out.push_back("{\"id\":"+quote(l.id)+",\"name\":"+quote(l.name)+",\"items\":"+std::to_string(l.items)+'}');
                return "{\"libraries\":"+join(out)+'}';
            }
            if(method=="library.uninstall" || method=="library.delete") {
                const bool uninstall=method=="library.uninstall";
                fields(p,{uninstall?"library":"id"});const auto id=std::string(string(member(p,uninstall?"library":"id")));
                try{if(uninstall)c.uninstallLibrary(id);else c.deleteLibraryItem(id);}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            // The plugin store (RFC 0006).
            if(method=="plugins.list") {
                fields(p,{});std::vector<std::string> out;
                for(const auto& x:c.installedPlugins()){
                    std::vector<std::string> platforms,used;
                    for(const auto& s:x.platforms)platforms.push_back(quote(s));
                    for(const auto& s:x.usedBy)used.push_back(quote(s));
                    out.push_back("{\"format\":"+quote(x.format)+",\"id\":"+quote(x.id)+",\"version\":"+quote(x.version)+",\"name\":"+quote(x.name)+",\"platforms\":"+join(platforms)+
                                  ",\"bytes\":"+std::to_string(x.bytes)+",\"usedBy\":"+join(used)+",\"open\":"+(x.open?"true":"false")+'}');
                }
                return "{\"plugins\":"+join(out)+'}';
            }
            if(method=="plugins.remove" || method=="plugins.addBuild") {
                const bool add=method=="plugins.addBuild";
                if(add)fields(p,{"format","id","version","platform","path"});else fields(p,{"format","id","version","platform"});
                const auto format=std::string(string(member(p,"format"))),id=std::string(string(member(p,"id"))),version=std::string(string(member(p,"version")));
                const auto platform=member(p,"platform")?std::string(string(member(p,"platform"))):std::string();
                try{if(add)c.addPluginBuild(format,id,version,platform,std::string(string(member(p,"path"))));else c.removePlugin(format,id,version,platform);}
                catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            // The trust store (RFC 0006).
            if(method=="trust.list") {
                fields(p,{});std::vector<std::string> out;
                try{for(const auto& k:c.trustedKeys()){std::vector<std::string> scope;for(const auto& s:k.scope)scope.push_back(quote(s));
                    out.push_back("{\"id\":"+quote(k.id)+",\"key\":"+quote(k.key)+",\"name\":"+quote(k.name)+",\"scope\":"+join(scope)+",\"revoked\":"+(k.revoked?"true":"false")+'}');}}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return "{\"keys\":"+join(out)+'}';
            }
            if(method=="trust.set" || method=="trust.remove") {
                const bool set=method=="trust.set";
                if(set)fields(p,{"key","name","scope","revoked"});else fields(p,{"id"});
                Control::TrustedKey k;
                if(set){k.key=std::string(string(member(p,"key")));k.name=std::string(string(member(p,"name")));
                    for(auto s:array(member(p,"scope"),3,0))k.scope.push_back(std::string(string(s)));
                    k.revoked=member(p,"revoked") && boolean(member(p,"revoked"));}
                try{if(set)c.setTrustedKey(k);else c.removeTrustedKey(std::string(string(member(p,"id"))));}
                catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            // Signing keys: names, IDs and public keys, never the private key.
            if(method=="keys.list" || method=="keys.make") {
                auto entry=[](const Control::SigningKey& k){return "{\"name\":"+quote(k.name)+",\"id\":"+quote(k.id)+",\"key\":"+quote(k.key)+'}';};
                try{
                    if(method=="keys.make"){fields(p,{"name"});return entry(c.makeSigningKey(std::string(string(member(p,"name")))));}
                    fields(p,{});std::vector<std::string> out;
                    for(const auto& k:c.signingKeys())out.push_back(entry(k));
                    return "{\"keys\":"+join(out)+'}';
                }catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
            }
            if(method=="library.tag") {
                fields(p,{"tag","hidden","color"});
                try{c.libraryTag(std::string(string(member(p,"tag"))),member(p,"hidden") && boolean(member(p,"hidden")),member(p,"color")?std::string(string(member(p,"color"))):std::string());}
                catch(const std::invalid_argument& e){throw Failure{-32602,e.what()};}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="library.edit") {
                fields(p,{"id","name","description","tags","color"});Control::PerformanceInfo info;
                info.name=std::string(string(member(p,"name")));info.description=std::string(string(member(p,"description")));
                for(auto t:array(member(p,"tags"),32,0))info.tags.push_back(std::string(string(t)));
                if(member(p,"color"))info.color=std::string(string(member(p,"color")));
                try{c.libraryEdit(std::string(string(member(p,"id"))),info);}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="library.copy") {
                fields(p,{"id"});
                try{return "{\"id\":"+quote(c.libraryCopy(std::string(string(member(p,"id")))))+'}';}catch(const std::exception& e){throw Failure{-32005,e.what()};}
            }
            if(method=="library.favorite") {
                fields(p,{"id","favorite"});
                try{c.libraryFavorite(std::string(string(member(p,"id"))),boolean(member(p,"favorite")));}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return R"({"accepted":true})";
            }
            if(method=="library.rescan") {
                fields(p,{});
                try{return "{\"items\":"+std::to_string(c.libraryRescan())+'}';}catch(const std::exception& e){throw Failure{-32005,e.what()};}
            }
            if(method=="parts.saveSound") {
                fields(p,{"part","name","new"});Control::SoundSaved saved;
                try{saved=c.savePartAsSound(integer(member(p,"part")),member(p,"name")?std::string(string(member(p,"name"))):std::string(),member(p,"new")?boolean(member(p,"new")):false);}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                std::vector<std::string> left;for(const auto& l:saved.left)left.push_back(quote(l));
                return "{\"id\":"+quote(saved.id)+",\"revision\":"+quote(saved.revision)+",\"left\":"+join(left)+'}';
            }
            if(method=="parts.loadSound") {
                fields(p,{"part","id"});std::vector<std::string> flagged;
                try{for(const auto& f:c.loadSound(integer(member(p,"part")),std::string(string(member(p,"id")))))flagged.push_back(quote(f));}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return "{\"accepted\":true,\"flagged\":"+join(flagged)+'}';
            }
            if(method=="performance.contents") {
                fields(p,{"directory"});std::vector<std::string> parts,scenes;
                try{const auto held=c.performanceContents(std::string(string(member(p,"directory"))));
                    for(const auto& x:held.parts)parts.push_back("{\"id\":"+std::to_string(x.id)+",\"name\":"+quote(x.name)+",\"bus\":"+(x.bus?"true":"false")+'}');
                    for(const auto& x:held.scenes)scenes.push_back("{\"id\":"+std::to_string(x.id)+",\"name\":"+quote(x.name)+'}');}
                catch(const std::exception& e){throw Failure{-32005,e.what()};}
                return "{\"parts\":"+join(parts)+",\"scenes\":"+join(scenes)+'}';
            }
            if(method=="performance.import") {
                fields(p,{"directory","parts","scenes"});std::vector<uint32_t> chosen,scenes;
                if(member(p,"parts"))for(auto x:array(member(p,"parts"),256,0))chosen.push_back(integer(x));
                if(member(p,"scenes"))for(auto x:array(member(p,"scenes"),256,0))scenes.push_back(integer(x));
                Control::Imported r;
                try{r=c.importPerformance(std::string(string(member(p,"directory"))),chosen,scenes);}catch(const std::exception& e){throw Failure{-32005,e.what()};}
                std::vector<std::string> parts,notes;for(const auto id:r.parts)parts.push_back(std::to_string(id));for(const auto& n:r.notes)notes.push_back(quote(n));
                return "{\"parts\":"+join(parts)+",\"notes\":"+join(notes)+'}';
            }
            if(method=="parts.fromSound") {
                fields(p,{"id"});
                try{return "{\"part\":"+std::to_string(c.partFromSound(std::string(string(member(p,"id")))))+'}';}catch(const std::exception& e){throw Failure{-32005,e.what()};}
            }
            if(method=="library.save" || method=="library.load") {
                if(method=="library.save")fields(p,{"name","new"});else fields(p,{"id"});
                try{
                    if(method=="library.save")return "{\"id\":"+quote(c.savePerformanceToLibrary(member(p,"name")?std::string(string(member(p,"name"))):std::string(),member(p,"new")?boolean(member(p,"new")):false))+'}';
                    c.loadPerformanceFromLibrary(std::string(string(member(p,"id"))));return R"({"loaded":true})";
                }catch(const std::exception& e){throw Failure{-32005,e.what()};}
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
            if(method=="window.info") {
                fields(p,{"node"});return std::string("{\"available\":")+(c.hasPluginWindow(node(c,member(p,"node")))?"true}":"false}");
            }
            if(method=="window.show") {
                fields(p,{"node","show","title"});const auto n=node(c,member(p,"node"));
                const bool show=boolean(member(p,"show"));
                try{c.showPluginWindow(n,show,member(p,"title")?std::string(string(member(p,"title"))):std::string());}
                catch(const std::runtime_error& e){throw Failure{-32005,e.what()};}
                return show?R"({"shown":true})":R"({"shown":false})";
            }
            if(method=="presets.list") {
                fields(p,{"node"});std::vector<std::string> values;const auto n=node(c,member(p,"node"));
                std::vector<PresetInfo> listed;
                try{listed=c.presets(n);}catch(const std::runtime_error& e){throw Failure{-32005,e.what()};} // why there are none
                for(const auto& v:listed)values.push_back("{\"uri\":"+quote(v.uri)+",\"name\":"+quote(v.name)+'}');
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
                        ",\"noteInputs\":" + std::to_string(n.noteInputs) + ",\"noteOutputs\":" + std::to_string(n.noteOutputs) +
                        ",\"enabled\":" + (graph.disabled.count(n.id) ? "false" : "true") + ",\"ringing\":" + (graph.ringing.count(n.id) ? "true" : "false") +
                        ",\"role\":" + (graph.roles.count(n.id) ? quote(graph.roles.at(n.id)) : std::string("null")) +
                        ",\"parameterRevision\":" + std::to_string(graph.parameterRevisions.at(n.id)) + '}');
                auto edges = [&](const auto& list, const char* kind) {
                    for( const auto& [from,to] : list ) connections.push_back("{\"kind\":" + quote(kind) +
                        ",\"from\":" + std::to_string(from) + ",\"to\":" + std::to_string(to) +
                        ",\"draining\":" + (graph.topology.draining.count({from,to}) ? "true" : "false") + '}');
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
            if( method == "graph.enable" ) { fields(p,{"node","enabled"}); c.setNodeEnabled(node(c,member(p,"node")),boolean(member(p,"enabled"))); return R"({"accepted":true})"; }
            if( method == "graph.role" ) { fields(p,{"node","role"}); const auto r = member(p,"role"); c.setNodeRole(node(c,member(p,"node")), r && r->type == json_type_null ? std::string() : std::string(string(r))); return R"({"accepted":true})"; }
            if( method == "master.set" ) { fields(p,{"level","pan"}); c.setMaster(number(member(p,"level")),number(member(p,"pan"))); return R"({"accepted":true})"; }
            if(method=="graph.replace")
            {
                fields(p,{"node","path","pluginId","preset"});
                const auto path=std::string(string(member(p,"path")));
                if(path.empty() || path.find('\0')!=std::string::npos)invalid("Invalid plugin path");
                try{c.replace(node(c,member(p,"node")),path,std::string(string(member(p,"pluginId"))),member(p,"preset")?std::string(string(member(p,"preset"))):std::string{});}
                catch(const std::runtime_error& e){throw Failure{-32004,e.what()};}
                return "{\"revision\":"+std::to_string(c.graphState().revision)+'}';
            }
            if( method == "harmony.get" )
            {
                fields(p,{});
                std::vector<std::string> parts, scales;
                for( const auto& part : c.graphState().parts ) parts.push_back("{\"part\":" + std::to_string(part.id) + ",\"key\":" + (part.key ? keyJson(*part.key) : std::string("null")) + '}');
                for( const auto& s : padScales() ) scales.push_back(quote(s.first));
                return "{\"key\":" + keyJson(c.key()) + ",\"parts\":" + join(parts) + ",\"scales\":" + join(scales) + '}';
            }
            if( method == "harmony.set" )
            {
                fields(p,{"part","key"});
                const auto key = member(p,"key");
                try
                {
                    if( !member(p,"part") ) c.setKey(keyFrom(key));
                    else c.setPartKey(integer(member(p,"part")), key && key->type == json_type_null ? std::optional<MusicalKey>{} : keyFrom(key));
                }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
                return R"({"accepted":true})";
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
                    if( method == "controls.add" ) { fields(p,{"name","part"}); return "{\"control\":" + std::to_string(c.addMacro(member(p,"name") ? std::string(string(member(p,"name"))) : std::string(), member(p,"part") ? integer(member(p,"part")) : 0)) + '}'; }
                    if( method == "controls.remove" ) { fields(p,{"control"}); c.removeMacro(integer(member(p,"control"))); return R"({"accepted":true})"; }
                    if( method == "controls.name" ) { fields(p,{"control","name"}); c.setMacroName(integer(member(p,"control")),std::string(string(member(p,"name")))); return R"({"accepted":true})"; }
                    if( method == "controls.targets" ) { fields(p,{"control","targets"}); c.setMacroTargets(integer(member(p,"control")),targetsFrom(c,member(p,"targets"))); return R"({"accepted":true})"; }
                    if( method == "controls.set" ) { fields(p,{"control","value"}); const auto v = number(member(p,"value")); if( v < 0 || v > 1 ) invalid("value is 0..1"); c.setMacro(integer(member(p,"control")),v); return R"({"accepted":true})"; }
                }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
            }
            // Physical controls' overrides (RFC 0011), by the control's logical identity.
            if( method == "assignments.get" )
            {
                fields(p,{});
                std::vector<std::string> out;
                for( const auto& o : c.assignments() )
                    out.push_back("{\"control\":" + quote(o.control) + ",\"kind\":\"" + assignmentKinds[size_t(o.target.kind)] + "\",\"node\":" + std::to_string(o.target.node) +
                                  ",\"param\":" + std::to_string(o.target.param) + ",\"id\":" + std::to_string(o.target.id) + ",\"part\":" + std::to_string(o.part) + '}');
                return "{\"assignments\":" + join(out) + '}';
            }
            if( method == "assignments.set" )
            {
                fields(p,{"control","kind","node","param","id","part"});
                Assignment t;
                const auto kind = string(member(p,"kind"));
                const auto at = std::find_if(assignmentKinds.begin(), assignmentKinds.end(), [&](const char* k) { return kind == k; });
                if( at == assignmentKinds.end() ) invalid("kind is none, parameter, control, partLevel, partMute, scene or panic");
                t.kind = Assignment::Kind(at - assignmentKinds.begin());
                if( t.kind == Assignment::Kind::Parameter ) { t.node = node(c,member(p,"node")); t.param = integer(member(p,"param")); }
                else if( t.kind != Assignment::Kind::None && t.kind != Assignment::Kind::Panic ) t.id = integer(member(p,"id"));
                try { c.assign(std::string(string(member(p,"control"))),t,member(p,"part") ? integer(member(p,"part")) : 0); }
                catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
                return R"({"accepted":true})";
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
                    if( method == "scenes.scope" )
                    {
                        fields(p,{"scene","scope"});
                        const auto list = member(p,"scope");
                        if( !list || list->type != json_type_array ) invalid("scope is an array of kinds");
                        uint32_t scope = 0;
                        for( auto* e = static_cast<const json_array_s*>(list->payload)->start; e; e = e->next )
                        {
                            const auto name = std::string(string(e->value));
                            const auto at = std::find(sceneKinds.begin(), sceneKinds.end(), name);
                            if( at == sceneKinds.end() ) invalid("a kind is enabled, mutes, mix, parameters or mappings");
                            scope |= 1u << (at - sceneKinds.begin());
                        }
                        c.setSceneScope(integer(member(p,"scene")), scope);
                        return R"({"accepted":true})";
                    }
                    if( method == "scenes.transition" )
                    {
                        fields(p,{"scene","glide","curve","launch"});
                        auto pick = [&](const char* key, const auto& names) {
                            const auto v = member(p,key);
                            if( !v ) return size_t(0);
                            const auto name = std::string(string(v));
                            const auto at = std::find(names.begin(), names.end(), name);
                            if( at == names.end() ) invalid("curve is linear, ease or equal-power; launch is now, beat or bar");
                            return size_t(at - names.begin());
                        };
                        const auto glide = member(p,"glide") ? integer(member(p,"glide")) : 0;
                        c.setSceneTransition(integer(member(p,"scene")), glide, Scene::Curve(pick("curve", sceneCurves)), Scene::Launch(pick("launch", sceneLaunches)));
                        return R"({"accepted":true})";
                    }
                    if( method == "scenes.untouched" )
                    {
                        fields(p,{"parameters"});
                        const auto list = member(p,"parameters");
                        if( !list || list->type != json_type_array ) invalid("parameters is an array of {node, param}");
                        std::vector<SceneParameter> parameters;
                        for( auto* e = static_cast<const json_array_s*>(list->payload)->start; e; e = e->next )
                        {
                            fields(e->value,{"node","param"});
                            if( parameters.size() == 65536 ) invalid("At most 65536 untouched parameters");
                            parameters.push_back({node(c,member(e->value,"node")),integer(member(e->value,"param"))});
                        }
                        c.setSceneUntouched(parameters);
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
                    if( method == "parts.meters" || method == "wires.meters" )
                    {
                        std::vector<std::string> meters;
                        auto add = [&](const char* key, uint32_t id, double peak, const std::vector<double>& values) {
                            std::vector<std::string> channels;
                            for( const auto v : values ) channels.push_back( encode( v ) );
                            meters.push_back("{\"" + std::string(key) + "\":" + std::to_string(id) + ",\"peak\":" + encode(peak) + ",\"channels\":" + join(channels) + '}');
                        };
                        if( method == "parts.meters" ) for( const auto& m : c.partMeters() ) add("part", m.part, m.peak, m.channels);
                        else for( const auto& m : c.wireMeters() ) add("wire", m.wire, m.peak, m.channels);
                        return "{\"meters\":" + join(meters) + '}';
                    }
                    if( method == "parts.name" ) { fields(p,{"part","name"}); c.setPartName(integer(member(p,"part")),std::string(string(member(p,"name")))); return R"({"accepted":true})"; }
                    if( method == "parts.input" ) { fields(p,{"part","input"}); c.setPartInput(integer(member(p,"part")),inputFrom(member(p,"input"))); return R"({"accepted":true})"; }
                    if( method == "parts.midiOut" )
                    {
                        fields(p,{"part","midiOut"});const auto out=member(p,"midiOut");fields(out,{"devices","channel"});
                        PartMidiOut midiOut;
                        if( member(out,"devices") ) midiOut.devices = devicesFrom(member(out,"devices"));
                        if( member(out,"channel") ) midiOut.channel = uint8_t(integer(member(out,"channel"),16));
                        c.setPartMidiOut(integer(member(p,"part")),midiOut);
                        return R"({"accepted":true})";
                    }
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
                    if( method == "wires.enable" ) { fields(p,{"wire","enabled"}); c.setWireEnabled(integer(member(p,"wire")),boolean(member(p,"enabled"))); return R"({"accepted":true})"; }
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
                        ",\"flags\":" + std::to_string(v.flags) + ",\"module\":" + quote(v.module) +
                        ",\"widget\":" + quote(widgetName(v.widget)) + ",\"priority\":" + std::to_string(v.priority) + ",\"group\":" + std::to_string(v.group) +
                        (v.widget == OSCAIRN_WIDGET_RANGE ? ",\"rangeHigh\":" + std::to_string(v.rangeHigh) : std::string()) +
                        (v.role ? ",\"role\":" + quote(roleName(v.role)) : std::string()) + '}' );
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
                fields(p, {"playing","tempo","clockSource","clockOutputs"});
                const auto play = member(p,"playing"), tempo = member(p,"tempo"), source = member(p,"clockSource"), outputs = member(p,"clockOutputs");
                if( !play && !tempo && !source && !outputs ) invalid("No transport fields supplied");
                const auto clockOutputs = outputs ? devicesFrom(outputs) : std::vector<DeviceRule>{};
                const bool playing = play ? boolean(play) : false;
                const double bpm = tempo ? number(tempo) : 120;
                if( bpm < 1 || bpm > 1000 ) invalid("Tempo must be between 1 and 1000 BPM");
                const auto clock = source ? string(source) : "internal";
                if( clock != "internal" && clock != "midi" ) invalid("Unknown clock source");
                if( tempo ) c.setTempo(bpm);
                if( play ) c.setPlaying(playing);
                if( source ) c.setClockSource(clock == "internal" ? Transport::ClockSource::Internal : Transport::ClockSource::MidiClock);
                if( outputs ) c.setClockOutputs(clockOutputs);
                return R"({"accepted":true})";
            }
            if( method == "notes.panic" )
            {
                fields(p, {});
                c.allNotesOff();
                return R"({"accepted":true})";
            }
            if( method == "midi.ignore" )
            {
                fields(p, {"inputs"});
                std::vector<std::string> names;
                for(auto t:array(member(p,"inputs"),64,0))names.push_back(std::string(string(t)));
                c.ignoreMidiInputs(names);
                return R"({"accepted":true})";
            }
            if( method == "controllers.set" )
            {
                fields(p, {"groups","ports"});
                std::vector<ControlGroup> groups;
                for(auto g:array(member(p,"groups"),64,0))groups.push_back(controlGroup(g));
                std::vector<Control::ControllerPort> ports;
                for(auto x:array(member(p,"ports"),64,0))
                {
                    fields(x, {"input","device","port"});
                    const auto port = integer(member(x,"port"));
                    if(!port) invalid("ports count from 1");
                    ports.push_back({std::string(string(member(x,"input"))),std::string(string(member(x,"device"))),port});
                }
                c.setControllers(groups, ports);
                return R"({"accepted":true})";
            }
            if( method == "controllers.view" )
            {
                fields(p, {"part","screen","positions"});
                Control::ControlView view;
                if(member(p,"part"))view.part = integer(member(p,"part"));
                if(const auto s = member(p,"screen"))
                {
                    fields(s, {"name","params"});
                    ScreenPage page; page.name = std::string(string(member(s,"name")));
                    for(auto a:array(member(s,"params"),64,0))
                    {
                        fields(a, {"kind","node","param","id"});
                        Assignment t;
                        t.kind = Assignment::Kind(named(assignmentKinds, string(member(a,"kind")), "kind is parameter, control, partLevel, partMute or scene"));
                        if( t.kind == Assignment::Kind::Parameter ) { t.node = node(c,member(a,"node")); t.param = integer(member(a,"param")); }
                        else if( t.kind != Assignment::Kind::None ) t.id = integer(member(a,"id"));
                        page.params.push_back(t);
                    }
                    view.screen = std::move(page);
                }
                if(const auto list = member(p,"positions"))
                    for(auto x:array(list,64,0)) { fields(x, {"group","position"}); view.positions.push_back({integer(member(x,"group")), integer(member(x,"position"))}); }
                try { c.setControlView(view); } catch( const std::invalid_argument& e ) { throw Failure{-32003,e.what()}; }
                return R"({"accepted":true})";
            }
            if( method == "controllers.get" ) { fields(p, {}); return controllers(c); }
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
        else if(topic == "record")
        {
            if(enabled && !record_) { lastRecord_.reset(); recordStates_.clear(); }
            record_ = enabled;
        }
        else if(topic == "controls.changed")
        {
            if(enabled && !values_) { lastValues_.reset(); valuesSent_.reset(); }
            values_ = enabled;
        }
        else if(topic == "controllers")
        {
            if(enabled && !controllers_) { lastControllers_.reset(); lastControllersValue_.clear(); }
            controllers_ = enabled;
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
        // Values controls and faders play (RFC 0011's instant controls), only those that changed,
        // at most 60 times a second: what the views show and devices get back, with no graph read.
        if( values_ && due(lastValues_) )
        {
            const auto v = control_.liveValues();
            const auto* was = valuesSent_ ? &*valuesSent_ : nullptr;
            std::vector<std::string> controls, parts;
            for( const auto& [id, value] : v.controls )
                if( !was || std::find(was->controls.begin(), was->controls.end(), std::pair{id, value}) == was->controls.end() )
                    controls.push_back("{\"control\":" + std::to_string(id) + ",\"value\":" + encode(value) + '}');
            for( const auto& f : v.parts )
                if( !was || std::find(was->parts.begin(), was->parts.end(), f) == was->parts.end() )
                    parts.push_back("{\"part\":" + std::to_string(f.part) + ",\"level\":" + encode(f.level) + ",\"mute\":" + (f.mute ? "true" : "false") + ",\"pan\":" + encode(f.pan) + '}');
            const bool master = !was || was->masterLevel != v.masterLevel || was->masterPan != v.masterPan;
            // The interval runs from the last one sent, so a move after a pause goes at the next poll.
            if( !controls.empty() || !parts.empty() || master )
            {
                send("controls.changed", "{\"controls\":" + join(controls) + ",\"parts\":" + join(parts) +
                     (master ? ",\"master\":{\"level\":" + encode(v.masterLevel) + ",\"pan\":" + encode(v.masterPan) + '}' : std::string()) + '}');
                lastValues_ = now;
            }
            valuesSent_ = v;
        }
        // Controllers (RFC 0011's instant controls): positions and the control moved last, for learn.
        if( controllers_ && due(lastControllers_) )
        {
            auto state = controllers(control_);
            if( state != lastControllersValue_ ) { send("controllers",state); lastControllersValue_ = std::move(state); lastControllers_ = now; }
        }
        // Status carries CPU and memory figures that change on every look: 4 times a second is
        // enough for what it shows, and 60 cost every client tens of KB of JSON a second.
        if(status_ && (!lastStatus_ || now < *lastStatus_ || now - *lastStatus_ >= 250000000))
        {
            auto value = status(control_);
            if(value != lastStatusValue_) { send("status",value); lastStatusValue_ = std::move(value); }
            lastStatus_ = now;
        }
        // Recording (RFC 0020): the takes being written and each whose state changed since, at most
        // 10 times a second. Overruns, falling behind and errors are in each take, never dropped.
        if(record_ && (!lastRecord_ || now < *lastRecord_ || now - *lastRecord_ >= 100000000))
        {
            std::vector<std::string> takes;
            std::map<uint32_t,std::string> states;
            for(const auto& t : control_.recordTakes())
            {
                const bool live = t.state == "waiting" || t.state == "recording" || t.state == "finishing";
                const auto it = recordStates_.find(t.take);
                if(live || (it != recordStates_.end() && it->second != t.state)) takes.push_back(takeJson(t));
                if(live || it != recordStates_.end()) states[t.take] = t.state;
            }
            auto value = "{\"takes\":" + join(takes) + '}';
            if(!takes.empty() && value != lastRecordValue_) { send("record",value); lastRecordValue_ = std::move(value); }
            recordStates_ = std::move(states);
            lastRecord_ = now;
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
