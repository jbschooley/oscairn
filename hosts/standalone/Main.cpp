// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn standalone: the demo instrument in a desktop window, with a choice of panel
// layouts (View menu, or --layout NAME|FILE.json). Every MIDI input plays it.
//
//   oscairn-standalone [--profile NAME|path.json] [--layout NAME|path.json] [--plugins DIR]
//                      [--in-process | --connect SOCKET] [--play] [--input-mode automatic|mouse|touch]
//                      [--snapshot out.png [--size WxH] [--play SECONDS] [--view TAB]]
//
// --profile names the device it runs on (RFC 0024): a built-in profile (qemu-aarch64, kick,
// ignite) or a file; without it, the computer's own. A panel profile fills the display with no
// title bar, shows its layout and asks the audio device for its block size; --layout and the
// audio options override it for a run. --play starts the sequencer at launch. --input-mode overrides the saved preference for this run. --snapshot
// renders the window to a PNG and quits: at the layout's design size, or as in a window of
// --size; with --play SECONDS, the sequencer plays for that long first; with --view, the
// desktop shows that tab (Part, Mixer, Routing or Controls).

#include "DesktopShell.h"
#include "Engine.h"
#include "Layout.h"
#include "LibraryPane.h"
#include "Launcher.h"
#include "Panel.h"
#include "Profile.h"
#include "PluginBrowser.h"
#include "PadEditor.h"
#include "PartsView.h"
#include "PerformanceColors.h"
#include "Setlist.h"

#include <BinaryData.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdio>
#include <memory>

namespace oscairn::standalone
{
    namespace
    {
        struct NamedLayout
        {
            juce::String name; // the file name without .json
            juce::String json;
        };

        std::vector<NamedLayout> builtInLayouts()
        {
            std::vector<NamedLayout> layouts;
            for( int i = 0; i < BinaryData::namedResourceListSize; ++i )
            {
                int         size = 0;
                const auto* data = BinaryData::getNamedResource( BinaryData::namedResourceList[i], size );
                const auto  file = juce::String( BinaryData::getNamedResourceOriginalFilename( BinaryData::namedResourceList[i] ) );
                if( data && file.endsWith( ".json" ) )
                    layouts.push_back( { file.upToLastOccurrenceOf( ".json", false, false ), juce::String::fromUTF8( data, size ) } );
            }
            // A fixed order for the menu.
            const juce::StringArray order{ "desktop", "kick", "kick-screen", "ignite", "keys49" };
            std::sort( layouts.begin(), layouts.end(), [&]( const NamedLayout& a, const NamedLayout& b ) {
                return order.indexOf( a.name ) < order.indexOf( b.name );
            } );
            return layouts;
        }

        juce::String argument( const juce::StringArray& args, const juce::String& name )
        {
            const auto i = args.indexOf( name );
            return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
        }
    }

    /// An app in a window of its own, apart from the sound editor; closing it hides it.
    class AppWindow final : public juce::DocumentWindow {
    public:
        AppWindow(const juce::String& name, juce::Component* content, int width, int height) : DocumentWindow("Oscairn - " + name, ink(Ink::Background), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true); setContentOwned(content, false); setResizable(true, false); centreWithSize(width, height);
        }
        std::function<void()> onClose;
        void closeButtonPressed() override { if(onClose) onClose(); else setVisible(false); }
    };

    class LibraryWindow final : public juce::DocumentWindow {
    public:
        LibraryWindow(Engine& engine,Theme& theme,const juce::File& plugins):DocumentWindow("Oscairn - Plugins and appearance",ink(Ink::Background),DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);setContentOwned(new PluginBrowser(engine,theme,plugins),true);
            setResizable(true,false);setResizeLimits(900,620,1800,1200);centreWithSize(1060,760);setVisible(true);
        }
        void closeButtonPressed() override {setVisible(false);}
    };

    class Window : public juce::DocumentWindow, public juce::MenuBarModel
    {
    public:
        Window( Engine& engine, std::vector<NamedLayout> layouts, const Layout& first, Profile profile,Theme& theme,const juce::File& plugins, InputMode mode )
            : DocumentWindow( "Oscairn", juce::Colours::black, DocumentWindow::allButtons )
            , engine_( engine )
            , layouts_( std::move( layouts ) )
            , profile_( std::move( profile ) )
            , fullScreen_( profile_.screen() && profile_.screen()->surface != "desktop" ),theme_(theme),plugins_(plugins),inputMode_(mode)
        {
            const bool fullScreen = fullScreen_; // a device's screen: no title bar, no menu bar
            setUsingNativeTitleBar( !fullScreen );
            if( fullScreen )
                setTitleBarHeight( 0 );
#if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu( this );
#else
            if( !fullScreen ) // a device's screen has no menu bar
                setMenuBar( this );
#endif
            engine_.openBrowser=[this]{showLibrary();};
            engine_.openPads=[this]{showPads();};
            engine_.openParts=[this]{showParts();};
            engine_.goHome=[this]{showHome();};
            engine_.goBack=[this]{showHome();}; // shortcut: every app's top screen is one level down; deeper levels go back inside their apps

            // What the built-in apps offer each other (RFC 0022): the Library picks, the Setlist takes.
            actions_.offer(pickPerformance,"Library",[this](const std::string&,Actions::Answer answer){showPicker(std::move(answer),"performance");});
            actions_.offer(pickSound,"Library",[this](const std::string&,Actions::Answer answer){showPicker(std::move(answer),"sound");});
            actions_.offer(sendPerformance,"Setlist",[this](const std::string& id,Actions::Answer answer){answer(setlist().add(id).toStdString());});
            show( first );
            setVisible( true );
        }

        ~Window() override
        {
            engine_.openBrowser={};
            engine_.openPads={};
            engine_.openParts={};
            engine_.goHome={};
            engine_.goBack={};
#if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu( nullptr );
#else
            setMenuBar( nullptr );
#endif
        }

        void show( const Layout& layout )
        {
            pads_.reset();parts_.reset();
            layout_ = layout;
            // Home first (RFC 0030): the window's content on the desktop, over a panel's screen.
            if( layout.surface != Layout::Surface::Desktop )
                setContentOwned( new Panel( engine_, layout ), false );
            showHome();
            setName( "Oscairn - " + juce::String( layout.name ) );

            // Start at the layout's design size (one design unit per pixel) if it fits,
            // otherwise as large as fits.
            const auto area  = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.reduced( 40 );
            const auto scale = juce::jmin( 1.0f, area.getWidth() / (float)layout.width, area.getHeight() / (float)layout.height );
            setResizable( layout.resize != Layout::Resize::Fixed, false );
            if( auto* c = getConstrainer() )
                c->setFixedAspectRatio( layout.resize == Layout::Resize::Scale ? layout.width / (double)layout.height : 0.0 );
            setResizeLimits( layout.width / 4, layout.height / 4, layout.width * 3, layout.height * 3 );
            if( fullScreen_ )
            {
                // The whole display, whatever its shape: the panel scales or stretches to fit.
                setResizeLimits( 1, 1, 1 << 15, 1 << 15 );
                if( auto* c = getConstrainer() )
                    c->setFixedAspectRatio( 0.0 );
                setBounds( juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->logicalBounds.toNearestInt() );
                return;
            }
            centreWithSize( juce::roundToInt( layout.width * scale ), juce::roundToInt( layout.height * scale ) );
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

        void resized() override
        {
            DocumentWindow::resized();
            if( pads_ && getContentComponent() ) pads_->setBounds( getContentComponent()->getBounds() );
            if( parts_ && parts_->getParentComponent() == this && getContentComponent() ) parts_->setBounds( getContentComponent()->getBounds() ); // a panel places its own
        }

        // Cmd-Z undoes and Cmd-Shift-Z redoes (Ctrl elsewhere), from any view: keys the views do
        // not use reach the window. Not Ctrl-Y: the on-screen keyboard plays Y as a note.
        bool keyPressed( const juce::KeyPress& key ) override
        {
            const auto mods = key.getModifiers();
            const int code = key.getKeyCode();
            if( !mods.isCommandDown() || mods.isAltDown() ) return false;
            if( (code == 'Z' || code == 'z') && !mods.isShiftDown() ) { engine_.edit( []( Control& c ) { c.undo(); } ); return true; }
            if( (code == 'Z' || code == 'z') && mods.isShiftDown() ) { engine_.edit( []( Control& c ) { c.redo(); } ); return true; }
            return false;
        }

        const Layout& layout() const { return layout_; }

        // Home (RFC 0030): the launcher, as the window's content on the desktop, over the screen
        // on a panel. What it replaces goes after it is in place: a panel points at its overlay.
        void showHome()
        {
            if( auto* p = panel() )
            {
                if( !launcher_ ) { launcher_ = std::make_unique<Launcher>( engine_, launcherApps() ); setDesktopMode( *launcher_, 2 ); }
                p->overlay( launcher_.get() );
                parts_.reset(); frame_.reset(); panelApp_.reset();
                return;
            }
            auto* home = new Launcher( engine_, launcherApps() );
            setDesktopMode( *home, inputMode_ == InputMode::Touch ? 2 : 1 );
            setContentOwned( home, false );
        }
        // The Performance app: playing and editing the open Performance. Its live-controls view
        // is not built yet, so it opens the parts editor.
        void openPerformance()
        {
            if( panel() ) { showParts(); return; }
            auto* shell = new DesktopShell( engine_, scanner(), inputMode_ );
            shell->onHome = [this] { showHome(); };
            shell->onSave = [this] { saveToLibrary( false ); };
            setContentOwned( shell, false );
        }
        DesktopShell* shell() const { return dynamic_cast<DesktopShell*>( getContentComponent() ); }

        juce::StringArray getMenuBarNames() override { return { "Performance", "Edit", "View", "Plugins", "Appearance", "Apps", "Part" }; }

        juce::PopupMenu getMenuForIndex( int index, const juce::String& ) override
        {
            juce::PopupMenu menu;
            if(index==0){const bool free=engine_.connected() && !engine_.soundBusy();menu.addItem(5002,"Save",free);menu.addItem(5003,"Save As New...",free);
                menu.addItem(5004,"Import Performance...",free && actions_.offered(pickPerformance));
                menu.addItem(5005,"Export Performance...",free);
                menu.addItem(5006,"Install Library...",free);
                // The color lists and setlists show it in; saved with the Performance.
                if(const auto info=engine_.performanceInfo()){juce::PopupMenu colors;addColors(colors,info->color,5100,"None");menu.addSubMenu("Color",colors,free);}
                menu.addSeparator();menu.addItem(5000,"Save to Folder...",free);menu.addItem(5001,"Open Folder...",free);return menu;}
            if(index==1){const auto h=engine_.history();
                menu.addItem(6000,h.undo.empty()?juce::String("Undo"):"Undo "+juce::String::fromUTF8(h.undo.c_str()),!h.undo.empty());
                menu.addItem(6001,h.redo.empty()?juce::String("Redo"):"Redo "+juce::String::fromUTF8(h.redo.c_str()),!h.redo.empty());return menu;}
            if(index==3){menu.addItem(2000,"Browse plugins and edit parameters...");return menu;}
            // Apps that play the sounds, each in its own window, apart from the sound editor.
            // The selected part's Sound (RFC 0021): from the library, or saved to it.
            if(index==6){const bool part=shell() && shell()->selectedPart() && engine_.connected() && !engine_.soundBusy();
                // Update when the library has a newer revision of the Sound the part came from.
                PartSource from;if(part)for(const auto& p:engine_.graph().parts)if(p.id==shell()->selectedPart())from=p.from;
                bool newer=false;
                if(part && !from.sound.empty())if(const auto item=engine_.libraryItem(from.sound))newer=!item->id.empty() && item->revision!=from.revision;
                menu.addItem(8100,"Load Sound into Part...",part && actions_.offered(pickSound));
                menu.addItem(8104,"Update from Library",newer);
                menu.addSeparator();
                menu.addItem(8101,from.sound.empty()?juce::String("Save Sound..."):"Save Sound",part);
                menu.addItem(8102,"Save Sound As New...",part);
                return menu;}
            // The apps this device has (its profile's).
            if(index==5){
                menu.addItem(8003,"Home");menu.addSeparator();
                menu.addItem(8004,"Performance");
                if(profile_.offers("oscairn.groovebox"))menu.addItem(8000,"Groovebox");
                if(profile_.offers("oscairn.library"))menu.addItem(8001,"Library");
                if(profile_.offers("oscairn.setlist"))menu.addItem(8002,"Setlist");
                return menu;}
            if(index==4){themeItems_=theme_.installed();for(size_t i=0;i<themeItems_.size();++i)menu.addItem(3000+int(i),themeItems_[i].second,true,themeItems_[i].first==themeData().id);menu.addSeparator();menu.addItem(4000,"Manage themes...");return menu;}
            for( size_t i = 0; i < layouts_.size(); ++i )
            {
                const auto& l      = layouts_[i];
                const auto  ticked = juce::String( layout_.name ).equalsIgnoreCase( title( l ) );
                menu.addItem( (int)i + 1, title( l ), true, ticked );
            }
            menu.addSeparator();
            menu.addItem( 1000, "Open Layout File..." );
            // How the desktop is used: control sizes and spacing (RFC 0019).
            menu.addSeparator();
            juce::PopupMenu input;
            for( auto mode : { InputMode::Automatic, InputMode::Mouse, InputMode::Touch } )
                input.addItem( 7000 + int( mode ), inputModeName( mode ), true, mode == inputMode_ );
            menu.addSubMenu( "Input", input, layout_.surface == Layout::Surface::Desktop );
            return menu;
        }

        void menuItemSelected( int id, int ) override
        {
            if(id==5000 || id==5001) {
                const bool save=id==5000;
                const auto initial=save?juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Untitled.oscairn"):juce::File();
                chooser_=std::make_unique<juce::FileChooser>(save?"Save Performance to a new folder":"Open Performance folder",initial,save?"*.oscairn":"*");
                chooser_->launchAsync((save?juce::FileBrowserComponent::saveMode:juce::FileBrowserComponent::openMode)|juce::FileBrowserComponent::canSelectDirectories,
                    [safe=juce::Component::SafePointer<Window>(this),save](const juce::FileChooser& fc) {
                        if(!safe || fc.getResult()==juce::File())return;
                        SoundAction action;action.kind=save?SoundAction::SavePerformance:SoundAction::LoadPerformance;
                        action.directory=fc.getResult().getFullPathName().toStdString();safe->engine_.soundAction(std::move(action));
                    });return;
            }
            if(id>=7000 && id<=7002){inputMode_=InputMode(id-7000);writeInputMode(theme_.configuration(),inputMode_);if(auto* s=shell())s->setInputMode(inputMode_);return;}
            if(id==8000){openGroovebox();return;}
            if(id==8001){showLibraryApp();return;}
            if(id==8002){showSetlist();return;}
            if(id==8003){showHome();return;}
            if(id==8004){openPerformance();return;}
            if(id==5002 || id==5003){saveToLibrary(id==5003);return;}
            if(id==5004){importPerformance();return;}
            if(id==5005){exportPerformance();return;}
            if(id==5006){showApp(libraryApp_,"Library",[this]{return libraryPane(LibraryPane::Mode::Play);},900,700);
                if(auto* pane=dynamic_cast<LibraryPane*>(libraryApp_->getContentComponent()))pane->install();return;}
            if(id==8100){loadSound();return;}
            if(id==8101 || id==8102){saveSound(id==8102);return;}
            if(id==8104){updateSound();return;}
            if(std::string color;colorChosen(id,5100,color)){engine_.edit([color](Control& c){auto info=c.performanceInfo();info.color=color;c.setPerformanceInfo(info);});return;}
            if(id==6000){engine_.edit([](Control& c){c.undo();});return;}
            if(id==6001){engine_.edit([](Control& c){c.redo();});return;}
            if(id==2000 || id==4000){showLibrary();return;}
            if(id>=3000 && size_t(id-3000)<themeItems_.size()){try{theme_.apply(themeItems_[size_t(id-3000)].first);}catch(const std::exception& e){juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,"Theme not applied",e.what());}return;}
            if( id == 1000 )
            {
                chooser_ = std::make_unique<juce::FileChooser>( "Open a panel layout", juce::File(), "*.json" );
                chooser_->launchAsync( juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                       [this]( const juce::FileChooser& fc ) {
                                           if( fc.getResult() != juce::File() )
                                               tryShow( fc.getResult().loadFileAsString() );
                                       } );
            }
            else if( id >= 1 && id <= (int)layouts_.size() )
                tryShow( layouts_[(size_t)id - 1].json );
        }

    private:
        void showGroovebox(){showApp(groovebox_,"Groovebox",[this]{return new SequencerPane(engine_);},1100,760);}
        Panel* panel() const { return dynamic_cast<Panel*>( getContentComponent() ); }
        bool layoutBinds( const std::string& bind ) const
        {
            return std::any_of( layout_.controls.begin(), layout_.controls.end(), [&]( const Layout::Control& c ) { return c.bind == bind; } );
        }
        // An app on a panel's screen, in its frame: Home on screen only where no button is Home.
        // The frame goes before the app it shows, and both after the new ones are in place.
        void openOnScreen( const juce::String& name, juce::Component& app, std::unique_ptr<juce::Component> owned )
        {
            auto frame = std::make_unique<AppFrame>( name, app, !layoutBinds( "home" ), [this] { showHome(); } );
            setDesktopMode( *frame, 2 ); setDesktopMode( app, 2 );
            panel()->overlay( frame.get() );
            parts_.reset();
            frame_ = std::move( frame );
            panelApp_ = std::move( owned );
        }
        // The apps Home offers: the Performance always, the rest as the profile has them.
        std::vector<Launcher::App> launcherApps()
        {
            std::vector<Launcher::App> apps;
            apps.push_back( { "Performance", "piano", [this] {
                const auto info = engine_.performanceInfo();
                return info ? juce::String::fromUTF8( info->name.empty() ? "Untitled" : info->name.c_str() ) + ": play or edit it" : juce::String(); },
                [this] { openPerformance(); } } );
            if( profile_.offers( "oscairn.library" ) )
                apps.push_back( { "Library", "library-big", [this] {
                    Control::LibraryQuery q; q.limit = 1; q.kind = "performance";
                    const auto performances = engine_.library( q ); q.kind = "sound"; const auto sounds = engine_.library( q );
                    if( !performances || !sounds ) return juce::String();
                    auto count = []( uint32_t n, const char* one, const char* many ) { return juce::String( int( n ) ) + " " + ( n == 1 ? one : many ); };
                    return count( performances->total, "Performance", "Performances" ) + ", " + count( sounds->total, "Sound", "Sounds" ); },
                    [this] { showLibraryApp(); } } );
            if( profile_.offers( "oscairn.setlist" ) )
                apps.push_back( { "Setlist", "list-music", [] { return juce::String( "Songs in order, for the stage" ); }, [this] { showSetlist(); } } );
            if( profile_.offers( "oscairn.groovebox" ) )
                apps.push_back( { "Groovebox", "layout-grid", [] { return juce::String( "Patterns and steps" ); }, [this] { openGroovebox(); } } );
            return apps;
        }
        void showLibraryApp()
        {
            if( panel() ) { std::unique_ptr<juce::Component> pane( libraryPane( LibraryPane::Mode::Play ) ); auto& app = *pane; openOnScreen( "Library", app, std::move( pane ) ); return; }
            showApp( libraryApp_, "Library", [this] { return libraryPane( LibraryPane::Mode::Play ); }, 900, 700 );
        }
        void showSetlist()
        {
            if( panel() ) { openOnScreen( "Setlist", setlist(), nullptr ); return; }
            if( !setlist_ ) { setlist_ = std::make_unique<AppWindow>( "Setlist", nullptr, 700, 760 ); setlist_->setContentNonOwned( &setlist(), false ); }
            showApp( setlist_, "Setlist", nullptr, 0, 0 );
        }
        void openGroovebox()
        {
            if( panel() ) { auto pane = std::make_unique<SequencerPane>( engine_, false ); auto& app = *pane; openOnScreen( "Groovebox", app, std::move( pane ) ); return; } // a panel has Play and Stop
            showGroovebox();
        }
        // An app's window, made the first time; it keeps its state while hidden.
        void showApp(std::unique_ptr<AppWindow>& window,const juce::String& name,std::function<juce::Component*()> make,int width,int height)
        {
            if(!window)window=std::make_unique<AppWindow>(name,make(),width,height);
            if(auto* content=window->getContentComponent())setDesktopMode(*content,inputMode_==InputMode::Touch?2:1);
            window->setVisible(true);window->toFront(true);
        }
        // The Setlist app, made hidden when another app sends it a Performance before it was opened.
        SetlistPane& setlist()
        {
            if(!setlistPane_){setlistPane_=std::make_unique<SetlistPane>(engine_,actions_);setDesktopMode(*setlistPane_,inputMode_==InputMode::Touch?2:1);}
            return *setlistPane_;
        }
        // A Library pane auditioning Sounds in the part the Part view shows.
        LibraryPane* libraryPane(LibraryPane::Mode mode,const std::string& kind="performance")
        {
            auto* pane=new LibraryPane(engine_,actions_,mode,kind);
            pane->auditionPart=[this]{auto* s=shell();return s?s->selectedPart():0u;};
            return pane;
        }
        // The Library's pick, in a window over the asking app; a second pick cancels the first.
        void showPicker(Actions::Answer answer,const std::string& kind)
        {
            if(pickerAnswer_){auto old=std::move(pickerAnswer_);pickerAnswer_={};old({});}
            pickerAnswer_=std::move(answer);
            auto* pane=libraryPane(LibraryPane::Mode::Pick,kind);
            pane->onPicked=[this](const std::string& id){
                auto pending=std::move(pickerAnswer_);pickerAnswer_={};
                // The window goes after this click has finished with it.
                juce::MessageManager::callAsync([safe=juce::Component::SafePointer<Window>(this)]{if(safe && !safe->pickerAnswer_)safe->picker_.reset();});
                if(pending)pending(id);
            };
            picker_=std::make_unique<AppWindow>(kind=="sound"?"Choose a Sound":"Choose a Performance",pane,760,620);
            picker_->onClose=[pane]{pane->onPicked({});};
            setDesktopMode(*pane,inputMode_==InputMode::Touch?2:1);
            picker_->setVisible(true);picker_->toFront(true);
        }
        // Another Performance's parts and scenes into this one (RFC 0021): the Library picks it,
        // then its parts and scenes are listed, all ticked; the chosen ones come, parts with what
        // they need. Everything ticked is all of it.
        void importPerformance()
        {
            actions_.request(pickPerformance,{},[safe=juce::Component::SafePointer<Window>(this)](const std::string& id){
                if(!safe || id.empty())return;
                const auto item=safe->engine_.libraryItem(id);
                if(!item || item->id.empty())return;
                const auto held=safe->engine_.performanceContents(item->path);
                if(!held)return;
                struct Choose : juce::Component
                {
                    juce::OwnedArray<juce::ToggleButton> parts,scenes;juce::Label partsHeading{{},"Parts"},scenesHeading{{},"Scenes"};
                    juce::TextButton import{"Import"},cancel{"Cancel"};
                    std::vector<uint32_t> partIds,sceneIds;
                    explicit Choose(const Control::PerformanceContents& held)
                    {
                        for(const auto& p:held.parts){auto* b=parts.add(new juce::ToggleButton(juce::String::fromUTF8(p.name.c_str())+(p.bus?" (bus)":"")));b->setToggleState(true,juce::dontSendNotification);addAndMakeVisible(b);partIds.push_back(p.id);}
                        for(const auto& s:held.scenes){auto* b=scenes.add(new juce::ToggleButton(juce::String::fromUTF8(s.name.c_str())));b->setToggleState(true,juce::dontSendNotification);addAndMakeVisible(b);sceneIds.push_back(s.id);}
                        for(auto* c:std::initializer_list<juce::Component*>{&partsHeading,&import,&cancel})addAndMakeVisible(c);
                        if(!held.scenes.empty())addAndMakeVisible(scenesHeading);
                        setSize(360,int(held.parts.size()+held.scenes.size())*30+(held.scenes.empty()?100:130));
                    }
                    void resized() override
                    {
                        auto area=getLocalBounds().reduced(16,12);
                        partsHeading.setBounds(area.removeFromTop(26));
                        for(auto* b:parts)b->setBounds(area.removeFromTop(30));
                        if(!scenes.isEmpty()){area.removeFromTop(4);scenesHeading.setBounds(area.removeFromTop(26));}
                        for(auto* b:scenes)b->setBounds(area.removeFromTop(30));
                        auto bar=area.removeFromBottom(30);
                        import.setBounds(bar.removeFromRight(100));bar.removeFromRight(8);cancel.setBounds(bar.removeFromRight(100));
                    }
                    static std::vector<uint32_t> ticked(const juce::OwnedArray<juce::ToggleButton>& boxes,const std::vector<uint32_t>& ids)
                    {
                        std::vector<uint32_t> out;for(int i=0;i<boxes.size();++i)if(boxes[i]->getToggleState())out.push_back(ids[size_t(i)]);
                        return out;
                    }
                };
                auto* content=new Choose(*held);
                juce::DialogWindow::LaunchOptions options;
                options.dialogTitle="Import from "+juce::String::fromUTF8(item->name.c_str());
                options.content.setOwned(content);options.componentToCentreAround=safe.getComponent();options.useNativeTitleBar=true;options.resizable=false;
                auto* dialog=options.launchAsync();
                content->cancel.onClick=[dialog]{dialog->exitModalState(0);};
                content->import.onClick=[safe,dialog,content,path=item->path]{
                    auto parts=Choose::ticked(content->parts,content->partIds),scenes=Choose::ticked(content->scenes,content->sceneIds);
                    const bool all=parts.size()==content->partIds.size() && scenes.size()==content->sceneIds.size();
                    // A partial import takes a part and, when there are any, a scene.
                    if(!all && (parts.empty() || (scenes.empty() && !content->sceneIds.empty()))){
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,"Import","Choose at least one part, and one scene: the scene sets how the parts come.");return;}
                    dialog->exitModalState(0);
                    if(!safe)return;
                    if(all){parts.clear();scenes.clear();} // all of it, loose plugins included
                    SoundAction import;import.kind=SoundAction::ImportPerformance;import.directory=path;import.parts=parts;import.scenes=scenes;safe->engine_.soundAction(std::move(import));
                };
            });
        }
        // The open Performance as one archive file to take to another device (RFC 0021): its
        // samples and its unedited parts' Sounds always go; edited parts' Sounds when asked.
        void exportPerformance()
        {
            const auto info=engine_.performanceInfo();
            const auto name=info && !info->name.empty()?juce::String::fromUTF8(info->name.c_str()):juce::String("Performance");
            chooser_=std::make_unique<juce::FileChooser>("Export the Performance",juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(name+".olib"),"*.olib");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting,
                [safe=juce::Component::SafePointer<Window>(this)](const juce::FileChooser& fc) {
                    if(!safe || fc.getResult()==juce::File())return;
                    auto file=fc.getResult();if(!file.hasFileExtension("olib"))file=file.withFileExtension("olib");
                    if(file.existsAsFile() && !file.deleteFile()){juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,"Not exported","Cannot replace "+file.getFullPathName());return;}
                    auto options=juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon).withTitle("Export "+file.getFileNameWithoutExtension())
                        .withMessage("Samples and the Sounds of parts unedited since they were loaded always go. The Sounds of edited parts can go too, as the library has them.")
                        .withButton("Export").withButton("With edited parts' Sounds").withButton("Cancel").withAssociatedComponent(safe.getComponent());
                    juce::AlertWindow::showAsync(options,[safe,path=file.getFullPathName().toStdString()](int chosen){
                        if(!safe || chosen==0)return; // 1: Export, 2: with edited Sounds, 0: Cancel
                        SoundAction out;out.kind=SoundAction::ExportPerformance;out.directory=path;out.asNew=chosen==2;
                        safe->engine_.soundAction(std::move(out));
                    });
                });
        }
        // The library's newer revision of the Sound the selected part came from, in its place.
        void updateSound()
        {
            const auto part=shell()?shell()->selectedPart():0;
            for(const auto& p:engine_.graph().parts)if(p.id==part && !p.from.sound.empty()){
                SoundAction load;load.kind=SoundAction::LoadSound;load.part=part;load.libraryId=p.from.sound;engine_.soundAction(std::move(load));}
        }
        // The Library picks a Sound; it replaces the selected part's chain, keeping the part's seat.
        void loadSound()
        {
            const auto part=shell()?shell()->selectedPart():0;
            if(!part)return;
            actions_.request(pickSound,{},[safe=juce::Component::SafePointer<Window>(this),part](const std::string& id){
                if(!safe || id.empty())return;
                SoundAction load;load.kind=SoundAction::LoadSound;load.part=part;load.libraryId=id;safe->engine_.soundAction(std::move(load));
            });
        }
        // Save writes the next revision of the Sound the part came from, asking for a name only
        // when it came from none; Save As New always asks, and makes another Sound.
        void saveSound(bool asNew)
        {
            const auto part=shell()?shell()->selectedPart():0;
            if(!part)return;
            std::string name;PartSource from;for(const auto& p:engine_.graph().parts)if(p.id==part){name=p.name;from=p.from;}
            if(!asNew && !from.sound.empty()){SoundAction save;save.kind=SoundAction::SaveSound;save.part=part;engine_.soundAction(std::move(save));return;}
            auto* w=new juce::AlertWindow(asNew?"Save Sound As New":"Save Sound","The Sound's name:",juce::MessageBoxIconType::NoIcon,this);
            w->addTextEditor("name",juce::String::fromUTF8(name.c_str()),"Name");
            w->addButton("Save",1,juce::KeyPress(juce::KeyPress::returnKey));w->addButton("Cancel",0,juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true,juce::ModalCallbackFunction::create([safe=juce::Component::SafePointer<Window>(this),w,part,asNew](int result){
                if(!safe || result!=1)return;
                SoundAction save;save.kind=SoundAction::SaveSound;save.part=part;save.asNew=asNew;save.choice.name=w->getTextEditorContents("name").trim().toStdString();
                safe->engine_.soundAction(std::move(save));
            }),true);
        }
        // Save writes over the open library Performance, asking for a name only when it has never
        // been saved there; Save As New always asks, and makes a new one.
        void saveToLibrary(bool asNew)
        {
            const auto info=engine_.performanceInfo();
            if(!info)return;
            if(!asNew && !info->id.empty()){SoundAction save;save.kind=SoundAction::SaveToLibrary;engine_.soundAction(std::move(save));return;}
            auto* w=new juce::AlertWindow(asNew?"Save As New":"Save","The Performance's name:",juce::MessageBoxIconType::NoIcon,this);
            w->addTextEditor("name",juce::String::fromUTF8(info->name.c_str()),"Name");
            w->addButton("Save",1,juce::KeyPress(juce::KeyPress::returnKey));w->addButton("Cancel",0,juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true,juce::ModalCallbackFunction::create([safe=juce::Component::SafePointer<Window>(this),w,asNew](int result){
                if(!safe || result!=1)return;
                SoundAction save;save.kind=SoundAction::SaveToLibrary;save.asNew=asNew;save.choice.name=w->getTextEditorContents("name").trim().toStdString();
                safe->engine_.soundAction(std::move(save));
            }),true);
        }
        void showLibrary(){if(!library_)library_=std::make_unique<LibraryWindow>(engine_,theme_,plugins_);library_->setVisible(true);library_->toFront(true);}
        // The pad editor covers the panel, so it fits a device's single screen too.
        void showPads()
        {
            pads_ = std::make_unique<PadEditor>( engine_ );
            pads_->onDone = [this] { juce::MessageManager::callAsync( [safe = juce::Component::SafePointer<Window>( this )] { if( safe ) safe->pads_.reset(); } ); };
            addAndMakeVisible( *pads_ );
            if( getContentComponent() ) pads_->setBounds( getContentComponent()->getBounds() );
        }
        static std::string scanner() { return juce::File::getSpecialLocation( juce::File::currentExecutableFile ).getSiblingFile( "oscairn-plugin-scan" ).getFullPathName().toStdString(); }
        void showParts()
        {
            if( !panel() && !shell() ) openPerformance(); // from Home: the Performance app has them
            if( auto* s = shell() ) { s->showTab( DesktopShell::Tab::Part ); return; } // the desktop has them in its window
            parts_ = std::make_unique<PartsView>( engine_, scanner() );
            parts_->onSave = [this] { saveToLibrary( false ); };
            setDesktopMode( *parts_, 2 ); // a device's screen: the type scale and targets for touch
            parts_->onDone = [this] { juce::MessageManager::callAsync( [safe = juce::Component::SafePointer<Window>( this )] {
                if( !safe ) return;
                safe->showHome(); // done editing: back Home, which lets go of the parts views
            } ); };
            // Over the device's screen, so its knobs and buttons stay in reach, as on the hardware.
            if( auto* panel = dynamic_cast<Panel*>( getContentComponent() ) ) { panel->overlay( parts_.get() ); return; }
            addAndMakeVisible( *parts_ );
            if( getContentComponent() ) parts_->setBounds( getContentComponent()->getBounds() );
        }
        static juce::String title( const NamedLayout& l )
        {
            try
            {
                return Layout::parse( l.json ).name;
            }
            catch( const std::exception& )
            {
                return l.name;
            }
        }

        void tryShow( const juce::String& json )
        {
            try
            {
                show( Layout::parse( json ) );
            }
            catch( const std::exception& e )
            {
                juce::AlertWindow::showMessageBoxAsync( juce::MessageBoxIconType::WarningIcon, "Layout not loaded", e.what() );
            }
        }

        Engine&                            engine_;
        std::vector<NamedLayout>           layouts_;
        std::unique_ptr<juce::FileChooser> chooser_;
        Profile                            profile_;
        bool                               fullScreen_;
        Theme& theme_;
        juce::File plugins_;
        std::unique_ptr<LibraryWindow> library_;
        Actions actions_;
        std::unique_ptr<SetlistPane> setlistPane_; // before its window, which shows it without owning it
        std::unique_ptr<Launcher> launcher_;       // Home on a panel
        std::unique_ptr<juce::Component> panelApp_; // the app on a panel's screen, if it owns it,
        std::unique_ptr<AppFrame> frame_;           // and its frame, which goes first
        std::unique_ptr<AppWindow> groovebox_, libraryApp_, setlist_, picker_;
        Actions::Answer pickerAnswer_;
        std::unique_ptr<PadEditor> pads_;
        std::unique_ptr<PartsView> parts_;
        std::vector<std::pair<juce::String,juce::String>> themeItems_;
        InputMode inputMode_;
        Layout layout_;
    };

    class Application : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override { return "Oscairn"; }
        const juce::String getApplicationVersion() override { return "0.1.0"; }
        bool               moreThanOneInstanceAllowed() override { return true; }

        void initialise( const juce::String& commandLine ) override
        {
            const auto args = juce::StringArray::fromTokens( commandLine, true );
            auto       dir  = argument( args, "--plugins" );
            if( dir.isEmpty() )
            {
                const auto binaryDir=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
                const auto bundled=binaryDir.getChildFile("plugins");
                const auto installed=binaryDir.getSiblingFile("lib").getChildFile("clap");
                dir=bundled.isDirectory()?bundled.getFullPathName():installed.isDirectory()?installed.getFullPathName():juce::String(OSCAIRN_PLUGIN_DIR);
            }
            try
            {
                theme_=std::make_unique<Theme>();
                host::AudioConfiguration audio;
                bool audioOptions=false;
                const juce::StringArray valued{"--plugins","--connect","--profile","--layout","--snapshot","--size","--view","--input-mode"};
                for(int i=0;i<args.size();++i)
                {
                    const auto name=args[i].toStdString();
                    if(host::AudioConfiguration::isOption(name) || valued.contains(args[i]))
                    {
                        if(i+1==args.size() || args[i+1].startsWith("--"))throw std::invalid_argument("Missing value for "+name);
                        const auto value=args[++i].unquoted().toStdString();
                        if(host::AudioConfiguration::isOption(name)){audio.setOption(name,value);audioOptions=true;}
                    }
                    else if(name=="--play") {if(i+1<args.size() && !args[i+1].startsWith("--"))++i;}
                    else if(name!="--in-process")throw std::invalid_argument("Unknown argument: "+name);
                }
                // The device this runs on (RFC 0024): its block size for a new engine, unless given.
                const auto profile=loadProfile(argument(args,"--profile").unquoted().toStdString());
                if(!args.contains("--connect") && !args.contains("--buffer-frames") && profile.audio.block>0)audio.bufferFrames=uint32_t(profile.audio.block);
                audio.validate();
                const auto socket = argument(args, "--connect");
                if(audioOptions && args.contains("--connect"))
                    throw std::invalid_argument("Audio options configure a new engine; omit --connect to use them");
                if(args.contains("--connect") && socket.isEmpty())
                    throw std::invalid_argument("--connect requires a socket path");
                if(args.contains("--in-process") && !socket.isEmpty())
                    throw std::invalid_argument("choose --in-process or --connect");
                std::string helper;
                if(socket.isEmpty() && !args.contains("--in-process"))
                    helper=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("oscairn-engine").getFullPathName().toStdString();
                engine_ = std::make_unique<Engine>( dir.unquoted().toStdString(), socket.unquoted().toStdString(), helper, audio );
                engine_->edit( [budget = Control::Budget{ profile.cpuBudget(), profile.memoryBudget() }]( Control& c ) { c.setBudget( budget ); } );

                auto layouts = builtInLayouts();
                auto wanted  = argument( args, "--layout" );
                if( wanted.isEmpty() )
                    wanted = profile.screen() ? juce::String( profile.screen()->layout ) : juce::String( "desktop" );
                juce::String json;
                for( const auto& l : layouts )
                    if( l.name == wanted )
                        json = l.json;
                if( json.isEmpty() )
                {
                    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile( wanted.unquoted() );
                    if( !file.existsAsFile() )
                        throw std::runtime_error( "no layout named " + wanted.toStdString() + " and no such file" );
                    json = file.loadFileAsString();
                }
                auto mode = readInputMode( theme_->configuration() );
                if( const auto m = argument( args, "--input-mode" ); m.isNotEmpty() )
                {
                    if( m == "automatic" ) mode = InputMode::Automatic;
                    else if( m == "mouse" ) mode = InputMode::Mouse;
                    else if( m == "touch" ) mode = InputMode::Touch;
                    else throw std::invalid_argument( "--input-mode must be automatic, mouse or touch" );
                }
                window_ = std::make_unique<Window>( *engine_, std::move( layouts ), Layout::parse( json ), profile,*theme_,juce::File(dir.unquoted()), mode );
                if( const auto view = argument( args, "--view" ); view.isNotEmpty() )
                {
                    DesktopShell::Tab tab;
                    if( !window_->shell() ) window_->openPerformance(); // from Home, as a tap on it would
                    if( !window_->shell() || !DesktopShell::tabNamed( view, tab ) )
                        throw std::invalid_argument( "--view needs the desktop layout and one of Part, Mixer, Routing, Controls" );
                    window_->shell()->showTab( tab );
                }
                if( args.contains( "--play" ) )
                    engine_->setPlaying( true );

                if( const auto snapshot = argument( args, "--snapshot" ); snapshot.isNotEmpty() )
                {
                    const auto seconds = argument( args, "--play" ).getDoubleValue();
                    const auto size = juce::StringArray::fromTokens( argument( args, "--size" ), "x", "" );
                    const auto w = size.size() == 2 ? size[0].getIntValue() : 0, h = size.size() == 2 ? size[1].getIntValue() : 0;
                    juce::Timer::callAfterDelay( juce::roundToInt( juce::jmax( 0.5, seconds ) * 1000.0 ), [this, snapshot, w, h] {
                        writeSnapshot( juce::File::getCurrentWorkingDirectory().getChildFile( snapshot ), w, h );
                        quit();
                    } );
                }
            }
            catch( const std::exception& e )
            {
                std::fprintf( stderr, "oscairn-standalone: %s\n", e.what() );
                setApplicationReturnValue( 1 );
                if(args.contains("--snapshot"))quit();
                else juce::AlertWindow::showAsync(
                    juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Oscairn could not start").withMessage(e.what()).withButton("Retry").withButton("Quit"),
                    [this,commandLine](int choice) {
                        if(choice==1)
                        {
                            window_.reset(); // the panel uses the engine
                            engine_.reset();
                            setApplicationReturnValue(0);
                            initialise(commandLine);
                        }
                        else quit();
                    });
            }
        }

        void shutdown() override
        {
            window_.reset(); // the panel uses the engine
            engine_.reset();
            theme_.reset();
        }

    private:
        void writeSnapshot( const juce::File& file, int width, int height )
        {
            auto* content = window_->getContentComponent();
            const auto& l = window_->layout();
            if( width > 0 && height > 0 )
                content->setSize( width, height );
            else
                content->setSize( l.width, l.height ); // one design unit per pixel
            const auto image = content->createComponentSnapshot( content->getLocalBounds(), true, 1.0f );
            file.deleteFile();
            juce::FileOutputStream out( file );
            if( !out.openedOk() || !juce::PNGImageFormat().writeImageToStream( image, out ) )
            {
                std::fprintf( stderr, "oscairn-standalone: could not write %s\n", file.getFullPathName().toRawUTF8() );
                setApplicationReturnValue( 1 );
            }
        }

        std::unique_ptr<Theme> theme_;
        std::unique_ptr<Engine> engine_;
        std::unique_ptr<Window> window_;
    };
}

START_JUCE_APPLICATION( oscairn::standalone::Application )
