// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panel.h"

#include <stdexcept>

namespace oscairn::standalone
{
    namespace
    {
        const juce::Colour face{ 0xff2a2e35 }, edge{ 0xff3a404a }, text{ 0xffd8dce2 }, dim{ 0xff8a929e }, accent{ 0xffff8a3d },
            lit{ 0xff3ddc97 };

        /// "synth:1" -> ("synth", 1); false if it is not of that form.
        bool splitParam( const std::string& bind, std::string& node, uint32_t& param )
        {
            const auto colon = bind.find( ':' );
            if( colon == std::string::npos || colon == 0 || colon + 1 == bind.size() )
                return false;
            node = bind.substr( 0, colon );
            try
            {
                size_t used = 0;
                param       = (uint32_t)std::stoul( bind.substr( colon + 1 ), &used );
                return used == bind.size() - colon - 1;
            }
            catch( const std::exception& )
            {
                return false;
            }
        }
    }

    /// A rotary knob bound to a parameter or the tempo.
    class Panel::Knob : public juce::Component
    {
    public:
        Knob( Engine& engine, const Layout::Control& c ) : engine_( engine ), label_( c.label )
        {
            slider_.setSliderStyle( juce::Slider::RotaryHorizontalVerticalDrag );
            slider_.setTextBoxStyle( juce::Slider::TextBoxBelow, true, 80, 18 );
            slider_.setColour( juce::Slider::rotarySliderFillColourId, accent );
            slider_.setColour( juce::Slider::rotarySliderOutlineColourId, edge );
            slider_.setColour( juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack );
            addAndMakeVisible( slider_ );

            if( c.bind == "tempo" )
            {
                tempo_ = true;
                slider_.setRange( 40.0, 240.0, 0.1 );
                slider_.setDoubleClickReturnValue( true, 120.0 );
                slider_.textFromValueFunction = []( double v ) { return juce::String( v, 1 ) + " BPM"; };
                slider_.onValueChange         = [this] { engine_.setTempo( slider_.getValue() ); };
                slider_.setValue( 120.0, juce::dontSendNotification );
            }
            else if( splitParam( c.bind, node_, param_ ) )
            {
                if( !engine.hasParam( node_, param_ ) )
                    throw std::runtime_error( "layout: knob \"" + c.label + "\" is bound to " + c.bind + ", which the engine does not have" );
                const auto info = engine.paramInfo( node_, param_ );
                slider_.setRange( info.minimum, info.maximum, info.stepped ? 1.0 : 0.0 );
                slider_.setDoubleClickReturnValue( true, info.defaultValue );
                slider_.textFromValueFunction = [this]( double v ) { return juce::String( engine_.paramText( node_, param_, v ) ); };
                slider_.onValueChange         = [this] { engine_.setParam( node_, param_, slider_.getValue() ); };
                if( label_.empty() )
                    label_ = info.name;
                refresh();
            }
            else if( !c.bind.empty() )
                throw std::runtime_error( "layout: knob \"" + c.label + "\" has an unknown binding \"" + c.bind + "\"" );
            else
            {
                slider_.setEnabled( false );
                slider_.setTextBoxStyle( juce::Slider::NoTextBox, true, 0, 0 );
            }
        }

        void refresh()
        {
            if( !node_.empty() && !slider_.isMouseButtonDown() )
                slider_.setValue( engine_.param( node_, param_ ), juce::dontSendNotification );
        }

        void paint( juce::Graphics& g ) override
        {
            g.setColour( slider_.isEnabled() ? text : dim );
            g.setFont( juce::FontOptions( 15.0f, juce::Font::bold ) );
            g.drawText( label_, getLocalBounds().removeFromTop( 20 ), juce::Justification::centred );
        }

        void resized() override { slider_.setBounds( getLocalBounds().withTrimmedTop( 20 ) ); }

    private:
        Engine&       engine_;
        std::string   label_, node_;
        uint32_t      param_ = 0;
        bool          tempo_ = false;
        juce::Slider  slider_;
    };

    /// A grid of pads that play notes; with `steps`, they light with the sequencer's playhead.
    class Panel::Pads : public juce::Component
    {
    public:
        Pads( Engine& engine, const Layout::Control& c ) : engine_( engine ), control_( c ) {}

        void setStep( int step )
        {
            if( step != step_ )
            {
                step_ = step;
                repaint();
            }
        }

        void paint( juce::Graphics& g ) override
        {
            for( int i = 0; i < control_.rows * control_.cols; ++i )
            {
                const auto r = pad( i );
                juce::Colour colour = face;
                if( i == held_ )
                    colour = accent;
                else if( control_.steps && i == step_ )
                    colour = lit.withAlpha( 0.8f );
                g.setColour( colour );
                g.fillRoundedRectangle( r, 6.0f );
                g.setColour( edge );
                g.drawRoundedRectangle( r, 6.0f, 1.5f );
                g.setColour( dim );
                g.setFont( juce::FontOptions( 13.0f ) );
                g.drawText( juce::String( i + 1 ), r.reduced( 8.0f ), juce::Justification::topLeft );
            }
        }

        void mouseDown( const juce::MouseEvent& e ) override
        {
            held_ = padAt( e.position );
            if( held_ >= 0 )
            {
                // Harder towards the top of the pad, like pressure on a velocity pad.
                const auto r        = pad( held_ );
                const auto velocity = juce::jlimit( 0.2f, 1.0f, 1.0f - ( e.position.y - r.getY() ) / r.getHeight() * 0.8f );
                engine_.note( true, control_.note + held_, velocity );
            }
            repaint();
        }

        void mouseUp( const juce::MouseEvent& ) override
        {
            if( held_ >= 0 )
                engine_.note( false, control_.note + held_, 0.0f );
            held_ = -1;
            repaint();
        }

    private:
        juce::Rectangle<float> pad( int i ) const
        {
            const auto w = getWidth() / (float)control_.cols, h = getHeight() / (float)control_.rows;
            return { ( i % control_.cols ) * w + 4.0f, ( i / control_.cols ) * h + 4.0f, w - 8.0f, h - 8.0f };
        }

        int padAt( juce::Point<float> p ) const
        {
            for( int i = 0; i < control_.rows * control_.cols; ++i )
                if( pad( i ).contains( p ) )
                    return i;
            return -1;
        }

        Engine&         engine_;
        Layout::Control control_;
        int             step_ = -1, held_ = -1;
    };

    /// A piano keyboard that plays notes.
    class Panel::Keys : public juce::Component, private juce::MidiKeyboardState::Listener
    {
    public:
        Keys( Engine& engine, const Layout::Control& c ) : engine_( engine )
        {
            state_.addListener( this );
            keyboard_.setAvailableRange( c.note, juce::jmin( 127, c.note + c.count - 1 ) );
            keyboard_.setLowestVisibleKey( c.note );
            keyboard_.setKeyPressBaseOctave( c.note / 12 );
            keyboard_.setOctaveForMiddleC( 4 );
            addAndMakeVisible( keyboard_ );
        }
        ~Keys() override { state_.removeListener( this ); }

        void resized() override
        {
            keyboard_.setBounds( getLocalBounds() );
            int whites = 0;
            for( int n = keyboard_.getRangeStart(); n <= keyboard_.getRangeEnd(); ++n )
                whites += juce::MidiMessage::isMidiNoteBlack( n ) ? 0 : 1;
            keyboard_.setKeyWidth( getWidth() / (float)juce::jmax( 1, whites ) );
        }

    private:
        void handleNoteOn( juce::MidiKeyboardState*, int, int note, float velocity ) override { engine_.note( true, note, velocity ); }
        void handleNoteOff( juce::MidiKeyboardState*, int, int note, float ) override { engine_.note( false, note, 0.0f ); }

        Engine&                       engine_;
        juce::MidiKeyboardState       state_;
        juce::MidiKeyboardComponent   keyboard_{ state_, juce::MidiKeyboardComponent::horizontalKeyboard };
    };

    /// The panel at its design size; Panel scales it to fit.
    class Panel::Board : public juce::Component
    {
    public:
        Board( Engine& engine, const Layout& layout ) : engine_( engine ), layout_( layout )
        {
            setSize( layout.width, layout.height );
            for( const auto& c : layout.controls )
            {
                using Type = Layout::Control::Type;
                std::unique_ptr<juce::Component> component;
                switch( c.type )
                {
                case Type::Screen:
                {
                    auto screen = std::make_unique<Screen>( engine );
                    screen_     = screen.get();
                    component   = std::move( screen );
                    break;
                }
                case Type::Knob:
                {
                    auto knob = std::make_unique<Knob>( engine, c );
                    knobs_.push_back( knob.get() );
                    component = std::move( knob );
                    break;
                }
                case Type::Button: component = button( c ); break;
                case Type::Pads:
                {
                    auto pads = std::make_unique<Pads>( engine, c );
                    pads_.push_back( pads.get() );
                    component = std::move( pads );
                    break;
                }
                case Type::Keys: component = std::make_unique<Keys>( engine, c ); break;
                case Type::Label: break; // drawn by paint()
                }
                if( component )
                {
                    addAndMakeVisible( *component );
                    placed_.emplace_back( component.get(), &c );
                    owned_.push_back( std::move( component ) );
                }
            }
            layOut( 0, 0 );
        }

        /// Sizes the board to the design plus `dx`, `dy` extra units, moving and sizing
        /// every element by its anchor.
        void layOut( int dx, int dy )
        {
            dx_ = dx;
            dy_ = dy;
            setSize( layout_.width + dx, layout_.height + dy );
            for( auto& [component, control] : placed_ )
                component->setBounds( control->anchor.place( control->rect, dx, dy ) );
            repaint();
        }

        void refresh()
        {
            const auto status = engine_.status();
            if( screen_ )
                screen_->refresh();
            for( auto* knob : knobs_ )
                knob->refresh();
            for( auto* pads : pads_ )
                pads->setStep( status.step );
            for( auto& [button, bind] : toggles_ )
            {
                const bool on = bind == "sync" ? engine_.followsMidiClock() : bind == "stop" ? !status.playing : status.playing;
                button->setToggleState( on, juce::dontSendNotification );
            }
        }

        void paint( juce::Graphics& g ) override
        {
            g.fillAll( layout_.background );
            for( const auto& p : layout_.panels )
            {
                const auto r = p.anchor.place( p.rect, dx_, dy_ );
                g.setColour( face.darker( 0.3f ) );
                g.fillRoundedRectangle( r.toFloat(), 8.0f );
                g.setColour( dim );
                g.setFont( juce::FontOptions( 13.0f, juce::Font::bold ) );
                g.drawText( p.label, r.withHeight( 20 ), juce::Justification::centred );
            }
            g.setColour( text );
            for( const auto& c : layout_.controls )
                if( c.type == Layout::Control::Type::Label )
                {
                    g.setFont( juce::FontOptions( (float)c.rect.getHeight() * 0.7f, juce::Font::bold ) );
                    g.drawText( c.label, c.anchor.place( c.rect, dx_, dy_ ), juce::Justification::centredLeft );
                }
        }

    private:
        std::unique_ptr<juce::Component> button( const Layout::Control& c )
        {
            auto b = std::make_unique<juce::TextButton>( c.label );
            b->setColour( juce::TextButton::buttonColourId, face );
            b->setColour( juce::TextButton::buttonOnColourId, accent.darker( 0.2f ) );
            b->setColour( juce::TextButton::textColourOffId, c.bind.empty() ? dim : text );
            if( c.bind == "play" )
                b->onClick = [this] { engine_.setPlaying( true ); };
            else if( c.bind == "stop" )
                b->onClick = [this] { engine_.setPlaying( false ); };
            else if( c.bind == "play-stop" )
                b->onClick = [this] { engine_.setPlaying( !engine_.status().playing ); };
            else if( c.bind == "sync" )
                b->onClick = [this] { engine_.setFollowMidiClock( !engine_.followsMidiClock() ); };
            else if( !c.bind.empty() )
                throw std::runtime_error( "layout: button \"" + c.label + "\" has an unknown binding \"" + c.bind + "\"" );
            if( !c.bind.empty() )
                toggles_.emplace_back( b.get(), c.bind );
            return b;
        }

        Engine&                                       engine_;
        const Layout&                                 layout_;
        std::vector<std::unique_ptr<juce::Component>> owned_;
        std::vector<std::pair<juce::Component*, const Layout::Control*>> placed_;
        int                                           dx_ = 0, dy_ = 0;
        Screen*                                       screen_ = nullptr;
        std::vector<Knob*>                            knobs_;
        std::vector<Pads*>                            pads_;
        std::vector<std::pair<juce::TextButton*, std::string>> toggles_;
    };

    Panel::Panel( Engine& engine, const Layout& layout ) : engine_( engine ), layout_( layout )
    {
        board_ = std::make_unique<Board>( engine, layout_ );
        addAndMakeVisible( *board_ );
        board_->refresh();
        startTimerHz( 30 );
    }

    Panel::~Panel() = default;

    void Panel::paint( juce::Graphics& g )
    {
        g.fillAll( layout_.background.darker( 0.4f ) );
    }

    void Panel::resized()
    {
        // Scale the board to fit the tighter dimension. A stretching layout takes the
        // space left in the other; otherwise the board is centred in it.
        const auto scale = juce::jmin( getWidth() / (float)layout_.width, getHeight() / (float)layout_.height );
        if( scale <= 0.0f )
            return;
        if( layout_.resize == Layout::Resize::Stretch )
        {
            board_->layOut( juce::jmax( 0, (int)( getWidth() / scale ) - layout_.width ), juce::jmax( 0, (int)( getHeight() / scale ) - layout_.height ) );
            board_->setTransform( juce::AffineTransform::scale( scale ) );
            return;
        }
        const auto x = ( getWidth() - layout_.width * scale ) / 2.0f, y = ( getHeight() - layout_.height * scale ) / 2.0f;
        board_->setTransform( juce::AffineTransform::scale( scale ).translated( x, y ) );
    }

    void Panel::timerCallback()
    {
        engine_.service();
        board_->refresh();
    }
}
