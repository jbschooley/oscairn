// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn standalone: the demo instrument in a desktop window, with a choice of panel
// layouts (View menu, or --layout NAME|FILE.json). Every MIDI input plays it.
//
//   oscairn-standalone [--layout NAME|path.json] [--plugins DIR] [--in-process | --connect SOCKET]
//                      [--fullscreen] [--play] [--input-mode automatic|mouse|touch]
//                      [--snapshot out.png [--size WxH] [--play SECONDS] [--view TAB]]
//
// --fullscreen fills the display with no title bar (for a device). --play starts the
// sequencer at launch. --input-mode overrides the saved preference for this run. --snapshot
// renders the window to a PNG and quits: at the layout's design size, or as in a window of
// --size; with --play SECONDS, the sequencer plays for that long first; with --view, the
// desktop shows that tab (Part, Mixer, Routing or Controls).

#include "DesktopShell.h"
#include "Engine.h"
#include "Layout.h"
#include "LibraryPane.h"
#include "Panel.h"
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
        Window( Engine& engine, std::vector<NamedLayout> layouts, const Layout& first, bool fullScreen,Theme& theme,const juce::File& plugins, InputMode mode )
            : DocumentWindow( "Oscairn", juce::Colours::black, DocumentWindow::allButtons )
            , engine_( engine )
            , layouts_( std::move( layouts ) )
            , fullScreen_( fullScreen ),theme_(theme),plugins_(plugins),inputMode_(mode)
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
            if( layout.surface == Layout::Surface::Desktop )
                setContentOwned( new DesktopShell( engine_, scanner(), inputMode_ ), false );
            else
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
        DesktopShell* shell() const { return dynamic_cast<DesktopShell*>( getContentComponent() ); }

        juce::StringArray getMenuBarNames() override { return { "Performance", "Edit", "View", "Plugins", "Appearance", "Apps", "Part" }; }

        juce::PopupMenu getMenuForIndex( int index, const juce::String& ) override
        {
            juce::PopupMenu menu;
            if(index==0){const bool free=engine_.connected() && !engine_.soundBusy();menu.addItem(5002,"Save",free);menu.addItem(5003,"Save As New...",free);
                menu.addItem(5004,"Import Performance...",free && actions_.offered(pickPerformance));
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
            if(index==5){menu.addItem(8000,"Groovebox");menu.addItem(8001,"Library");menu.addItem(8002,"Setlist");return menu;}
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
            if(id==8000){showGroovebox();return;}
            if(id==8001){showApp(libraryApp_,"Library",[this]{return new LibraryPane(engine_,actions_,LibraryPane::Mode::Play);},900,700);return;}
            if(id==8002){setlist();showApp(setlist_,"Setlist",nullptr,0,0);return;}
            if(id==5002 || id==5003){saveToLibrary(id==5003);return;}
            if(id==5004){importPerformance();return;}
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
            if(!setlist_){setlist_=std::make_unique<AppWindow>("Setlist",new SetlistPane(engine_,actions_),700,760);setDesktopMode(*setlist_->getContentComponent(),inputMode_==InputMode::Touch?2:1);}
            return *static_cast<SetlistPane*>(setlist_->getContentComponent());
        }
        // The Library's pick, in a window over the asking app; a second pick cancels the first.
        void showPicker(Actions::Answer answer,const std::string& kind)
        {
            if(pickerAnswer_){auto old=std::move(pickerAnswer_);pickerAnswer_={};old({});}
            pickerAnswer_=std::move(answer);
            auto* pane=new LibraryPane(engine_,actions_,LibraryPane::Mode::Pick,kind);
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
        // Another Performance's parts into this one (RFC 0021): the Library picks it, then its
        // parts are listed, all ticked; the chosen ones come with what they need.
        void importPerformance()
        {
            actions_.request(pickPerformance,{},[safe=juce::Component::SafePointer<Window>(this)](const std::string& id){
                if(!safe || id.empty())return;
                const auto item=safe->engine_.libraryItem(id);
                if(!item || item->id.empty())return;
                const auto parts=safe->engine_.performanceParts(item->path);
                if(!parts)return;
                struct Choose : juce::Component
                {
                    juce::OwnedArray<juce::ToggleButton> boxes;juce::TextButton import{"Import"},cancel{"Cancel"};
                    std::vector<uint32_t> ids;
                    Choose(const std::vector<Control::PerformancePart>& parts)
                    {
                        for(const auto& p:parts){auto* b=boxes.add(new juce::ToggleButton(juce::String::fromUTF8(p.name.c_str())+(p.bus?" (bus)":"")));b->setToggleState(true,juce::dontSendNotification);addAndMakeVisible(b);ids.push_back(p.id);}
                        addAndMakeVisible(import);addAndMakeVisible(cancel);
                        setSize(360,int(parts.size())*30+70);
                    }
                    void resized() override
                    {
                        auto area=getLocalBounds().reduced(16,12);
                        for(auto* b:boxes)b->setBounds(area.removeFromTop(30));
                        auto bar=area.removeFromBottom(30);
                        import.setBounds(bar.removeFromRight(100));bar.removeFromRight(8);cancel.setBounds(bar.removeFromRight(100));
                    }
                    // Ticked parts by their IDs in the file; all ticked is all of it, scenes included.
                    std::vector<uint32_t> chosen() const
                    {
                        std::vector<uint32_t> out;for(int i=0;i<boxes.size();++i)if(boxes[i]->getToggleState())out.push_back(ids[size_t(i)]);
                        return out.size()==ids.size()?std::vector<uint32_t>{}:out;
                    }
                };
                auto* content=new Choose(*parts);
                juce::DialogWindow::LaunchOptions options;
                options.dialogTitle="Import from "+juce::String::fromUTF8(item->name.c_str());
                options.content.setOwned(content);options.componentToCentreAround=safe.getComponent();options.useNativeTitleBar=true;options.resizable=false;
                auto* dialog=options.launchAsync();
                content->cancel.onClick=[dialog]{dialog->exitModalState(0);};
                content->import.onClick=[safe,dialog,content,path=item->path]{
                    const auto chosen=content->chosen();
                    const bool none=std::none_of(content->boxes.begin(),content->boxes.end(),[](auto* b){return b->getToggleState();});
                    dialog->exitModalState(0);
                    if(!safe || none)return;
                    SoundAction import;import.kind=SoundAction::ImportPerformance;import.directory=path;import.parts=chosen;safe->engine_.soundAction(std::move(import));
                };
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
            if( auto* s = shell() ) { s->showTab( DesktopShell::Tab::Part ); return; } // the desktop has them in its window
            parts_ = std::make_unique<PartsView>( engine_, scanner() );
            setDesktopMode( *parts_, 2 ); // a device's screen: the type scale and targets for touch
            parts_->onDone = [this] { juce::MessageManager::callAsync( [safe = juce::Component::SafePointer<Window>( this )] {
                if( !safe ) return;
                if( auto* panel = dynamic_cast<Panel*>( safe->getContentComponent() ) ) panel->overlay( nullptr );
                safe->parts_.reset();
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
        bool                               fullScreen_;
        Theme& theme_;
        juce::File plugins_;
        std::unique_ptr<LibraryWindow> library_;
        Actions actions_;
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
                const juce::StringArray valued{"--plugins","--connect","--layout","--snapshot","--size","--view","--input-mode"};
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
                auto mode = readInputMode( theme_->configuration() );
                if( const auto m = argument( args, "--input-mode" ); m.isNotEmpty() )
                {
                    if( m == "automatic" ) mode = InputMode::Automatic;
                    else if( m == "mouse" ) mode = InputMode::Mouse;
                    else if( m == "touch" ) mode = InputMode::Touch;
                    else throw std::invalid_argument( "--input-mode must be automatic, mouse or touch" );
                }
                window_ = std::make_unique<Window>( *engine_, std::move( layouts ), Layout::parse( json ), args.contains( "--fullscreen" ),*theme_,juce::File(dir.unquoted()), mode );
                if( const auto view = argument( args, "--view" ); view.isNotEmpty() )
                {
                    DesktopShell::Tab tab;
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
