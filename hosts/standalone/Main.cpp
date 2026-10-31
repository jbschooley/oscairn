// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn standalone: the demo instrument in a desktop window, with a choice of panel
// layouts (View menu, or --layout NAME|FILE.json). Every MIDI input plays it.
//
//   oscairn-standalone [--layout NAME|path.json] [--plugins DIR] [--in-process | --connect SOCKET]
//                      [--fullscreen] [--play] [--snapshot out.png [--size WxH] [--play SECONDS]]
//
// --fullscreen fills the display with no title bar (for a device). --play starts the
// sequencer at launch. --snapshot renders the panel to a PNG and quits: at its design size,
// or as in a window of --size; with --play SECONDS, the sequencer plays for that long first.

#include "Engine.h"
#include "Layout.h"
#include "Panel.h"
#include "PluginBrowser.h"
#include "PadEditor.h"
#include "PartsView.h"

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
        Window( Engine& engine, std::vector<NamedLayout> layouts, const Layout& first, bool fullScreen,Theme& theme,const juce::File& plugins )
            : DocumentWindow( "Oscairn", juce::Colours::black, DocumentWindow::allButtons )
            , engine_( engine )
            , layouts_( std::move( layouts ) )
            , fullScreen_( fullScreen ),theme_(theme),plugins_(plugins)
        {
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
            show( first );
            setVisible( true );
        }

        ~Window() override
        {
            engine_.openBrowser={};
            engine_.openPads={};
            engine_.openParts={};
#if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu( nullptr );
#else
            setMenuBar( nullptr );
#endif
        }

        void show( const Layout& layout )
        {
            pads_.reset();parts_.reset();
            setContentOwned( new Panel( engine_, layout ), false );
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
            if( parts_ && getContentComponent() ) parts_->setBounds( getContentComponent()->getBounds() );
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

        juce::StringArray getMenuBarNames() override { return { "Performance", "Edit", "View", "Plugins", "Appearance" }; }

        juce::PopupMenu getMenuForIndex( int index, const juce::String& ) override
        {
            juce::PopupMenu menu;
            if(index==0){menu.addItem(5000,"Save Performance As...",engine_.connected() && !engine_.soundBusy());menu.addItem(5001,"Open Performance...",engine_.connected() && !engine_.soundBusy());return menu;}
            if(index==1){const auto h=engine_.history();
                menu.addItem(6000,h.undo.empty()?juce::String("Undo"):"Undo "+juce::String::fromUTF8(h.undo.c_str()),!h.undo.empty());
                menu.addItem(6001,h.redo.empty()?juce::String("Redo"):"Redo "+juce::String::fromUTF8(h.redo.c_str()),!h.redo.empty());return menu;}
            if(index==3){menu.addItem(2000,"Browse plugins and edit parameters...");return menu;}
            if(index==4){themeItems_=theme_.installed();for(size_t i=0;i<themeItems_.size();++i)menu.addItem(3000+int(i),themeItems_[i].second,true,themeItems_[i].first==themeData().id);menu.addSeparator();menu.addItem(4000,"Manage themes...");return menu;}
            const auto*     current = dynamic_cast<Panel*>( getContentComponent() );
            for( size_t i = 0; i < layouts_.size(); ++i )
            {
                const auto& l      = layouts_[i];
                const auto  ticked = current && juce::String( current->layout().name ).equalsIgnoreCase( title( l ) );
                menu.addItem( (int)i + 1, title( l ), true, ticked );
            }
            menu.addSeparator();
            menu.addItem( 1000, "Open Layout File..." );
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
        void showLibrary(){if(!library_)library_=std::make_unique<LibraryWindow>(engine_,theme_,plugins_);library_->setVisible(true);library_->toFront(true);}
        // The pad editor covers the panel, so it fits a device's single screen too.
        void showPads()
        {
            pads_ = std::make_unique<PadEditor>( engine_ );
            pads_->onDone = [this] { juce::MessageManager::callAsync( [safe = juce::Component::SafePointer<Window>( this )] { if( safe ) safe->pads_.reset(); } ); };
            addAndMakeVisible( *pads_ );
            if( getContentComponent() ) pads_->setBounds( getContentComponent()->getBounds() );
        }
        void showParts()
        {
            const auto scanner = juce::File::getSpecialLocation( juce::File::currentExecutableFile ).getSiblingFile( "oscairn-plugin-scan" ).getFullPathName().toStdString();
            parts_ = std::make_unique<PartsView>( engine_, scanner );
            parts_->onDone = [this] { juce::MessageManager::callAsync( [safe = juce::Component::SafePointer<Window>( this )] { if( safe ) safe->parts_.reset(); } ); };
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
        bool                               fullScreen_;
        Theme& theme_;
        juce::File plugins_;
        std::unique_ptr<LibraryWindow> library_;
        std::unique_ptr<PadEditor> pads_;
        std::unique_ptr<PartsView> parts_;
        std::vector<std::pair<juce::String,juce::String>> themeItems_;
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
                const juce::StringArray valued{"--plugins","--connect","--layout","--snapshot","--size"};
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
                    else if(name!="--fullscreen" && name!="--in-process")throw std::invalid_argument("Unknown argument: "+name);
                }
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

                auto layouts = builtInLayouts();
                auto wanted  = argument( args, "--layout" );
                if( wanted.isEmpty() )
                    wanted = "desktop";
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
                window_ = std::make_unique<Window>( *engine_, std::move( layouts ), Layout::parse( json ), args.contains( "--fullscreen" ),*theme_,juce::File(dir.unquoted()) );
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
            auto* panel = dynamic_cast<Panel*>( window_->getContentComponent() );
            const auto& l = panel->layout();
            if( width > 0 && height > 0 )
                panel->setSize( width, height );
            else
                panel->setSize( l.width, l.height ); // one design unit per pixel
            const auto image = panel->createComponentSnapshot( panel->getLocalBounds(), true, 1.0f );
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
