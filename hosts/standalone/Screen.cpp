// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Screen.h"
#include "Theme.h"
#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace oscairn::standalone
{
    namespace
    {
        namespace ids=oscairn::sequencer;

        juce::String noteName( int note )
        {
            return juce::MidiMessage::getMidiNoteName( note, true, true, 4 ); // middle C (60) is C4
        }
    }

    Screen::Screen(Engine& engine) : engine_(engine)
    {
        for(auto* component : std::initializer_list<juce::Component*>{&sounds_, &useSound_, &prepareSound_, &removeSound_, &playingSound_, &soundNotice_, &brightnessLocks_, &plugins_, &tracks_, &channels_, &lengths_, &patterns_, &mute_, &launch_, &patternStatus_})
            addAndMakeVisible(component);
        engine_.selectTrack(0);
        tracks_.setName("Sequencer track");tracks_.addItem("Track 1",1);tracks_.addItem("Track 2",2);
        tracks_.setSelectedId(1,juce::dontSendNotification);
        tracks_.onChange=[this]{track_=uint32_t(tracks_.getSelectedId()-1);engine_.selectTrack(track_);refresh();};
        channels_.setName("Track MIDI channel");lengths_.setName("Pattern track length");
        for(int i=1;i<=16;++i){channels_.addItem("MIDI "+juce::String(i),i);lengths_.addItem(juce::String(i)+" steps",i);}
        channels_.onChange=[this]{engine_.setParam("sequencer",ids::channel(track_),channels_.getSelectedId()-1);};
        lengths_.onChange=[this]{engine_.setParam("sequencer",ids::length(pattern_,track_),lengths_.getSelectedId());};
        mute_.onClick=[this]{engine_.setParam("sequencer",ids::mute(track_),mute_.getToggleState()?1:0);};
        patterns_.setName("Pattern to edit");patterns_.addItem("Edit A",1);patterns_.addItem("Edit B",2);
        patterns_.setSelectedId(1,juce::dontSendNotification);
        patterns_.onChange=[this]{pattern_=uint32_t(patterns_.getSelectedId()-1);refresh();};
        launch_.setTooltip("Launch the edited pattern at the next bar, or immediately while stopped. Both tracks switch together.");
        launch_.onClick=[this]{engine_.setParam("sequencer",ids::requestedPattern,pattern_);};
        plugins_.onClick=[this]{if(engine_.openBrowser)engine_.openBrowser();};
        sounds_.setName("Prepared sounds");
        sounds_.setTextWhenNothingSelected("Choose a prepared sound");
        sounds_.onChange=[this]{refreshPicker();};
        brightnessLocks_.setTooltip("Override the sound's brightness on each sequencer step. Off by default so Warm and Bright keep their tone.");
        brightnessLocks_.onClick=[this] {
            auto action=selectedAction(SoundAction::BrightnessLocks);
            action.enableLocks=brightnessLocks_.getToggleState();engine_.soundAction(action);
        };
        useSound_.setTooltip("Use this sound for new notes on the selected track’s MIDI channel. Pads follow this track too. Held notes keep their sound.");
        useSound_.onClick=[this]{engine_.soundAction(selectedAction(SoundAction::Select));refreshPicker();};
        prepareSound_.onClick=[this] {
            const auto state=engine_.soundPicker();
            juce::PopupMenu menu;
            for(size_t i=0;i<state.choices.size();++i)menu.addItem(static_cast<int>(i)+1,state.choices[i].name);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&prepareSound_),
                [safe=juce::Component::SafePointer<Screen>(this),state](int id) {
                    if(!safe || id<=0 || static_cast<size_t>(id)>state.choices.size())return;
                    SoundAction action;action.kind=SoundAction::Prepare;action.node=state.node;
                    action.pluginId=state.pluginId;action.choice=state.choices[static_cast<size_t>(id)-1];
                    safe->engine_.soundAction(std::move(action));
                });
        };
        removeSound_.onClick=[this] {
            auto action=selectedAction(SoundAction::Discard);
            const auto& state=engine_.soundPicker();
            const auto found=std::find_if(state.sounds.contexts.begin(),state.sounds.contexts.end(),[&](const auto& s){return s.id==action.context;});
            if(found==state.sounds.contexts.end())return;
            if(found->idle){engine_.soundAction(action);return;}
            juce::AlertWindow::showAsync(juce::MessageBoxOptions().withTitle("Remove prepared sound")
                .withMessage("This sound may still have an effect tail. Remove it and stop that tail?")
                .withButton("Remove").withButton("Keep"),
                [safe=juce::Component::SafePointer<Screen>(this),action](int choice) mutable {
                    if(safe && choice==1){action.cutTail=true;safe->engine_.soundAction(action);}
                });
        };
    }

    SoundAction Screen::selectedAction(SoundAction::Kind kind) const
    {
        SoundAction action;action.kind=kind;action.node=engine_.soundPicker().node;
        action.pluginId=engine_.soundPicker().pluginId;action.track=track_;action.channel=static_cast<int16_t>(engine_.trackChannel());
        const auto index=sounds_.getSelectedItemIndex();
        if(index>=0 && static_cast<size_t>(index)<soundIds_.size())action.context=soundIds_[static_cast<size_t>(index)];
        return action;
    }

    void Screen::refreshPicker()
    {
        const auto& state=engine_.soundPicker();
        brightnessLocks_.setToggleState(state.brightnessLocks[track_],juce::dontSendNotification);
        brightnessLocks_.setEnabled(state.canToggleBrightnessLocks[track_] && engine_.connected() && !engine_.soundBusy());
        const auto oldIndex=sounds_.getSelectedItemIndex();
        const auto oldId=oldIndex>=0 && static_cast<size_t>(oldIndex)<soundIds_.size()?soundIds_[static_cast<size_t>(oldIndex)]:UINT32_MAX;
        std::vector<uint32_t> ids;juce::StringArray labels;juce::StringArray playing;
        for(const auto& sound:state.sounds.contexts)
        {
            ids.push_back(sound.id);labels.add(sound.name);
            if(sound.selectedChannels & (1u<<engine_.trackChannel()))playing.add(sound.name);
        }
        if(ids!=soundIds_ || labels!=soundLabels_)
        {
            soundIds_=std::move(ids);soundLabels_=labels;sounds_.clear(juce::dontSendNotification);
            sounds_.addItemList(labels,1);
            auto selected=std::find(soundIds_.begin(),soundIds_.end(),oldId);
            if(selected!=soundIds_.end())sounds_.setSelectedItemIndex(static_cast<int>(selected-soundIds_.begin()),juce::dontSendNotification);
            else if(!soundIds_.empty())sounds_.setSelectedItemIndex(static_cast<int>(soundIds_.size())-1,juce::dontSendNotification);
        }
        const auto id=selectedAction(SoundAction::Select).context;
        const auto found=std::find_if(state.sounds.contexts.begin(),state.sounds.contexts.end(),[&](const auto& s){return s.id==id;});
        const bool ready=engine_.connected() && !engine_.soundBusy() && found!=state.sounds.contexts.end();
        sounds_.setEnabled(!engine_.soundBusy() && engine_.connected());
        useSound_.setEnabled(ready && !(found->selectedChannels & (1u<<engine_.trackChannel())));
        removeSound_.setEnabled(ready && id!=0 && !found->selectedChannels && !found->activeVoices && (found->idle || state.sounds.mode=="instances"));
        prepareSound_.setEnabled(engine_.connected() && !engine_.soundBusy() && engine_.canPrepareSounds() &&
            !state.choices.empty() && state.sounds.contexts.size()<state.sounds.capacity);
        prepareSound_.setTooltip(engine_.canPrepareSounds()?"Prepare a sound without changing playback.":"Preparation needs the separate engine; relaunch without --in-process.");
        playingSound_.setText("Playing: "+(playing.isEmpty()?juce::String("none"):playing.joinIntoString(" / ")),juce::dontSendNotification);
        const auto notice=!engine_.soundMessage().empty()?juce::String(engine_.soundMessage()):
            engine_.soundBusy()?juce::String("Updating sounds..."):
            state.brightnessLocks[track_]?juce::String("Brightness locks override the sound on each step"):
            juce::String(state.sounds.contexts.size())+" / "+juce::String(state.sounds.capacity)+" prepared | Use sound changes this track’s channel";
        soundNotice_.setText(notice,juce::dontSendNotification);
        soundNotice_.setTooltip(notice);
    }

    void Screen::resized()
    {
        auto area=getLocalBounds().withTrimmedTop(juce::roundToInt(header().getHeight())).withHeight(juce::roundToInt(getHeight()*.20f)).reduced(10,0);
        auto title=area.removeFromTop(area.getHeight()/3);
        brightnessLocks_.setBounds(title.removeFromRight(title.getWidth()/3));
        plugins_.setBounds(title.removeFromRight(title.getWidth()/3).reduced(2));
        playingSound_.setBounds(title);
        auto row=area.removeFromTop(area.getHeight()/2);
        const int buttonWidth=row.getWidth()/6;
        removeSound_.setBounds(row.removeFromRight(buttonWidth).reduced(2));
        prepareSound_.setBounds(row.removeFromRight(buttonWidth).reduced(2));
        useSound_.setBounds(row.removeFromRight(buttonWidth).reduced(2));
        sounds_.setBounds(row.reduced(2));soundNotice_.setBounds(area);
        auto controls=getLocalBounds().withTrimmedTop(juce::roundToInt(getHeight()*.32f)).withHeight(juce::roundToInt(getHeight()*.14f)).reduced(10,0);
        auto trackRow=controls.removeFromTop(controls.getHeight()/2);const auto width=trackRow.getWidth()/4;
        tracks_.setBounds(trackRow.removeFromLeft(width).reduced(2));channels_.setBounds(trackRow.removeFromLeft(width).reduced(2));
        lengths_.setBounds(trackRow.removeFromLeft(width).reduced(2));mute_.setBounds(trackRow.reduced(2));
        patterns_.setBounds(controls.removeFromLeft(width).reduced(2));launch_.setBounds(controls.removeFromLeft(width).reduced(2));patternStatus_.setBounds(controls.reduced(2));
    }

    void Screen::refresh()
    {
        status_ = engine_.status();
        length_=static_cast<int>(engine_.param("sequencer",ids::length(pattern_,track_)));
        channels_.setSelectedId(engine_.trackChannel()+1,juce::dontSendNotification);
        lengths_.setSelectedId(length_,juce::dontSendNotification);
        mute_.setToggleState(engine_.param("sequencer",ids::mute(track_))>=.5,juce::dontSendNotification);
        const auto active=static_cast<int>(engine_.param("sequencer",ids::activePattern));
        const auto requested=static_cast<int>(engine_.param("sequencer",ids::requestedPattern));
        patternStatus_.setText(juce::String("Playing ")+(active?"B":"A")+(requested!=active?juce::String(" / Queued ")+(requested?"B":"A"):juce::String()),juce::dontSendNotification);
        for(auto* control:std::initializer_list<juce::Component*>{&channels_,&lengths_,&mute_,&launch_})control->setEnabled(engine_.connected());
        if(pattern_!=static_cast<uint32_t>(active))status_.step=-1;
        for( int i = 0; i < steps; ++i )
        {
            on_[(size_t)i]   = engine_.param( "sequencer", stepParameter(ids::On,i) ) >= 0.5;
            note_[(size_t)i] = (int)engine_.param( "sequencer", stepParameter(ids::Note,i) );
            lock_[(size_t)i] = engine_.param( "sequencer", stepParameter(ids::Lock,i) );
        }
        refreshPicker();
        repaint();
    }

    juce::Rectangle<float> Screen::header() const
    {
        return getLocalBounds().toFloat().removeFromTop( getHeight() * 0.12f );
    }

    juce::Rectangle<float> Screen::cell( int step ) const
    {
        auto       area = getLocalBounds().toFloat().withTrimmedTop( getHeight()*.46f ).reduced( getWidth() * 0.015f );
        const auto w = area.getWidth() / 8.0f, h = area.getHeight() / 2.0f;
        return juce::Rectangle<float>( area.getX() + ( step % 8 ) * w, area.getY() + ( step / 8 ) * h, w, h ).reduced( w * 0.05f );
    }

    juce::Rectangle<float> Screen::lockBar( int step ) const
    {
        auto c = cell( step );
        return c.removeFromBottom( c.getHeight() * 0.3f ).reduced( c.getWidth() * 0.12f, c.getHeight() * 0.04f );
    }

    int Screen::stepAt( juce::Point<float> p ) const
    {
        for( int i = 0; i < steps; ++i )
            if( cell( i ).contains( p ) )
                return i;
        return -1;
    }

    void Screen::paint( juce::Graphics& g )
    {
        g.fillAll( ink(Ink::Background) );
        const float unit = juce::jmin( getHeight() / 600.0f, getWidth() / 1024.0f ); // drawn for the KICK's 1024 x 600

        auto top = header().reduced( 14.0f * unit, 0.0f );
        g.setColour( ink(Ink::Text) );
        g.setFont( juce::FontOptions( 30.0f * unit, juce::Font::bold ) );
        g.drawText( "GROOVEBOX", top, juce::Justification::centredLeft );
        g.setFont( juce::FontOptions( 22.0f * unit ) );
        const auto midi = status_.midiInputs.empty() ? juce::String( "no MIDI inputs" )
                                                     : juce::String( (int)status_.midiInputs.size() ) + " MIDI in";
        const auto state = juce::String( status_.playing ? "PLAYING" : "STOPPED" ) + "   " + juce::String( status_.tempo, 1 ) + " BPM"
                         + ( engine_.followsMidiClock() ? "  (MIDI clock)" : "" ) + "   " + midi;
        g.setColour( status_.playing ? ink(Ink::Positive) : ink(Ink::Muted) );
        g.drawText( state, top, juce::Justification::centredRight );

        for( int i = 0; i < steps; ++i )
        {
            const auto c       = cell( i );
            const bool playing = i == status_.step;
            g.setColour( on_[(size_t)i] ? ink(Ink::Surface).brighter( 0.15f ) : ink(Ink::Surface) );
            g.fillRoundedRectangle( c, themeData().radius * unit );
            if( playing )
            {
                g.setColour( ink(Ink::Accent) );
                g.drawRoundedRectangle( c.reduced( 1.5f * unit ), themeData().radius * unit, 3.0f * unit );
            }

            auto inner = c.reduced( 10.0f * unit );
            g.setColour( ink(Ink::Muted) );
            g.setFont( juce::FontOptions( 18.0f * unit ) );
            g.drawText( juce::String( i + 1 ), inner.removeFromTop( 22.0f * unit ), juce::Justification::topLeft );
            g.setColour( on_[(size_t)i] ? ink(Ink::Text) : ink(Ink::Muted).darker( 0.5f ) );
            g.setFont( juce::FontOptions( 34.0f * unit, juce::Font::bold ) );
            g.drawText( on_[(size_t)i] ? noteName( note_[(size_t)i] ) : juce::String( "-" ), inner.withTrimmedBottom( c.getHeight() * 0.3f ),
                        juce::Justification::centred );

            const auto bar = lockBar( i );
            g.setColour( ink(Ink::Background) );
            g.fillRoundedRectangle( bar, themeData().radius*.5f * unit );
            g.setColour( on_[(size_t)i] && engine_.soundPicker().brightnessLocks[track_] ? ink(Ink::Accent).withAlpha( 0.85f ) : ink(Ink::Muted).withAlpha( 0.4f ) );
            g.fillRoundedRectangle( bar.withWidth( bar.getWidth() * (float)lock_[(size_t)i] ), themeData().radius*.5f * unit );
            if(i>=length_){g.setColour(ink(Ink::Background).withAlpha(.65f));g.fillRoundedRectangle(c,themeData().radius*unit);}
        }
    }

    uint32_t Screen::stepParameter(sequencer::StepKind kind,int step) const
    {return ids::step(pattern_,track_,kind,static_cast<uint32_t>(step));}

    void Screen::setLock( int step, float x )
    {
        const auto bar   = lockBar( step );
        const auto value = juce::jlimit( 0.0f, 1.0f, ( x - bar.getX() ) / bar.getWidth() );
        engine_.setParam( "sequencer", stepParameter(ids::Lock,step), value );
        refresh();
    }

    void Screen::mouseDown( const juce::MouseEvent& e )
    {
        dragStep_  = stepAt( e.position );
        dragMoved_ = false;
        if( dragStep_ < 0 )
            drag_ = Drag::None;
        else if( lockBar( dragStep_ ).expanded( 0.0f, 6.0f ).contains( e.position ) )
        {
            drag_ = Drag::Lock;
            setLock( dragStep_, e.position.x );
        }
        else
        {
            drag_     = Drag::Toggle; // becomes a note change if it moves
            dragNote_ = note_[(size_t)dragStep_];
        }
    }

    void Screen::mouseDrag( const juce::MouseEvent& e )
    {
        if( drag_ == Drag::Lock )
            setLock( dragStep_, e.position.x );
        else if( drag_ != Drag::None && std::abs( e.getDistanceFromDragStartY() ) > 4 )
        {
            drag_            = Drag::Note;
            const auto pitch = juce::jlimit( 0, 127, dragNote_ - (int)std::lround( e.getDistanceFromDragStartY() / ( 8.0f * getHeight() / 600.0f ) ) );
            engine_.setParam( "sequencer", stepParameter(ids::Note,dragStep_), pitch );
            refresh();
        }
    }

    void Screen::mouseUp( const juce::MouseEvent& )
    {
        if( drag_ == Drag::Toggle )
        {
            engine_.setParam( "sequencer", stepParameter(ids::On,dragStep_), on_[(size_t)dragStep_] ? 0.0 : 1.0 );
            refresh();
        }
        drag_ = Drag::None;
    }

    void Screen::mouseWheelMove( const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel )
    {
        const int step = stepAt( e.position );
        if( step < 0 || wheel.deltaY == 0.0f )
            return;
        const auto pitch = juce::jlimit( 0, 127, note_[(size_t)step] + ( wheel.deltaY > 0 ? 1 : -1 ) );
        engine_.setParam( "sequencer", stepParameter(ids::Note,step), pitch );
        refresh();
    }
}
