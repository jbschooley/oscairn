// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panel.h"
#include "Theme.h"

#include <stdexcept>

namespace oscairn::standalone
{
    namespace
    {
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
            slider_.setColour( juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack );
            if( c.endless )
            {
                // Its LEDs go most of the way round, as on hardware encoder rings.
                slider_.getProperties().set( "endless", true );
                slider_.setRotaryParameters( juce::MathConstants<float>::pi * 1.1f, juce::MathConstants<float>::pi * 2.9f, true );
            }
            addAndMakeVisible( slider_ );

            if( c.bind == "tempo" )
            {
                tempo_ = true;
                slider_.setRange( 40.0, 240.0, 0.1 );
                slider_.setDoubleClickReturnValue( true, 120.0 );
                slider_.textFromValueFunction = []( double v ) { return juce::String( v, 1 ) + " BPM"; };
                slider_.onValueChange         = [this] { engine_.setTempo( slider_.getValue() ); };
                refresh();
                slider_.updateText();
            }
            else if( splitParam( c.bind, node_, param_ ) )
            {
                pluginId_=node_=="synth"?"org.oscairn.basic-synth":engine.pluginId(node_);
                const auto info=engine.hasParam(node_,param_)?engine.paramInfo(node_,param_):Engine::ParamInfo{param_,"Unavailable",0,1,0};
                slider_.setRange( info.minimum, info.maximum, info.stepped ? 1.0 : 0.0 );
                slider_.setDoubleClickReturnValue( true, info.defaultValue );
                slider_.textFromValueFunction = [this]( double v ) { return juce::String( engine_.paramText( node_, param_, v ) ); };
                slider_.onValueChange         = [this] { engine_.setParam( node_, param_, slider_.getValue() ); };
                slider_.onDragStart           = [this] { engine_.paramGesture( node_, param_, true ); };
                slider_.onDragEnd             = [this] { engine_.paramGesture( node_, param_, false ); };
                if( label_.empty() )
                    label_ = info.name;
                refresh();
                slider_.updateText(); // zero may already be the slider's value before its formatter was installed
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
            if( slider_.isMouseButtonDown() )
                return;
            if( tempo_ )
                slider_.setValue( engine_.status().tempo, juce::dontSendNotification );
            else if( !node_.empty() ) {
                const bool available=engine_.hasParam(node_,param_) && engine_.pluginId(node_)==pluginId_;
                slider_.setEnabled(available);
                if(available && !available_) {
                    const auto info=engine_.paramInfo(node_,param_);
                    slider_.setRange(info.minimum,info.maximum,info.stepped?1.0:0.0);
                    slider_.setDoubleClickReturnValue(true,info.defaultValue);
                }
                available_=available;
                if(available){slider_.setValue(engine_.param(node_,param_),juce::dontSendNotification);if(engine_.controlReady())slider_.updateText();}
            }
        }

        void paint( juce::Graphics& g ) override
        {
            g.setColour( slider_.isEnabled() ? ink(Ink::Text) : ink(Ink::Muted) );
            g.setFont( juce::FontOptions( textHeight() * 0.8f*themeData().fontScale, juce::Font::bold ) );
            g.drawText( label_, getLocalBounds().removeFromTop( textHeight() ), juce::Justification::centred );
        }

        void resized() override
        {
            if( slider_.getTextBoxPosition() != juce::Slider::NoTextBox )
                slider_.setTextBoxStyle( juce::Slider::TextBoxBelow, true, getWidth(), textHeight() );
            slider_.setBounds( getLocalBounds().withTrimmedTop( textHeight() ) );
        }

    private:
        int textHeight() const { return juce::jlimit( 18, 48, getHeight() / 7 ); }

        Engine&       engine_;
        std::string   label_, node_, pluginId_;
        uint32_t      param_ = 0;
        bool          tempo_ = false, available_ = false;
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
                juce::Colour colour = ink(Ink::Raised);
                if( i == held_ )
                    colour = ink(Ink::Accent);
                else if( control_.steps && i == step_ )
                    colour = ink(Ink::Positive).withAlpha( 0.8f );
                g.setColour( colour );
                g.fillRoundedRectangle( r, themeData().radius );
                g.setColour( ink(Ink::Border) );
                g.drawRoundedRectangle( r, themeData().radius, 1.5f );
                g.setColour( i==held_ || (control_.steps && i==step_) ? colour.contrasting() : ink(Ink::Muted) );
                g.setFont( juce::FontOptions( 13.0f*themeData().fontScale ) );
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
            g.fillAll( ink(Ink::Background) );
            for( const auto& p : layout_.panels )
            {
                const auto r = p.anchor.place( p.rect, dx_, dy_ );
                g.setColour( ink(Ink::Raised).darker( 0.3f ) );
                g.fillRoundedRectangle( r.toFloat(), themeData().radius );
                g.setColour( ink(Ink::Muted) );
                g.setFont( juce::FontOptions( 13.0f*themeData().fontScale, juce::Font::bold ) );
                g.drawText( p.label, r.withHeight( 20 ), juce::Justification::centred );
            }
            g.setColour( ink(Ink::Text) );
            for( const auto& c : layout_.controls )
                if( c.type == Layout::Control::Type::Label )
                {
                    g.setFont( juce::FontOptions( (float)c.rect.getHeight() * 0.7f, juce::Font::bold ) );
                    g.drawText( c.label, c.anchor.place( c.rect, dx_, dy_ ), c.align );
                }
        }



    private:
        std::unique_ptr<juce::Component> button( const Layout::Control& c )
        {
            auto b = std::make_unique<juce::TextButton>( c.label );
            if(c.bind.empty())b->setEnabled(false);
            if(c.bind=="plugins")b->onClick=[this]{if(engine_.openBrowser)engine_.openBrowser();};
            else if( c.bind == "play" )
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

    Panel::Panel( Engine& engine, const Layout& layout ) : layout_( layout ),engine_(engine)
    {
        board_ = std::make_unique<Board>( engine, layout_ );
        addAndMakeVisible( *board_ );
        addChildComponent(connectionNotice_);addChildComponent(restartButton_);
        restartButton_.onClick=[this]{engine_.restart();timerCallback();};
        board_->refresh();
        startTimerHz( 30 );
    }

    Panel::~Panel() = default;

    void Panel::paint( juce::Graphics& g )
    {
        g.fillAll( ink(Ink::Background).darker( 0.4f ) );
    }

    void Panel::resized()
    {
        auto banner=getLocalBounds().removeFromTop(44).reduced(4);
        restartButton_.setBounds(banner.removeFromRight(150));
        connectionNotice_.setBounds(banner);
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
        board_->refresh();
        const auto disconnected=!engine_.connected();
        board_->setEnabled(!disconnected && !engine_.soundBusy());
        connectionNotice_.setColour(juce::Label::backgroundColourId,ink(Ink::Danger));
        connectionNotice_.setColour(juce::Label::textColourId,ink(Ink::Danger).contrasting());
        connectionNotice_.setVisible(disconnected);
        restartButton_.setVisible(disconnected && engine_.canRestart());
        connectionNotice_.setText(engine_.canRestart()?"Engine stopped. Restart opens a fresh session.":"Engine disconnected. Reconnecting...",juce::dontSendNotification);
        if(disconnected){connectionNotice_.toFront(false);restartButton_.toFront(false);}
    }
}
