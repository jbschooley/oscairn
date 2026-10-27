/*
 * Modular engine: the patch.
 *
 * A patch is a ValueTree, so the program can keep it in the project (saved with
 * it, undoable):
 *
 *   <MODULAR_PATCH name="Init" voices="8" macroName1="Brightness">
 *     <MODULE id="1" type="osc" col="0" row="0" wave="2" level="0.5"/>
 *     <MODULE id="3" type="vca" col="0" row="1" gain="0">
 *       <MOD param="gain" source="module:2" depth="1"/>
 *     </MODULE>
 *     <MODULE id="4" type="osc" col="1" row="0" input="1">
 *       <CABLE source="module:1" amount="0.5"/>
 *     </MODULE>
 *     <MODULE id="2" type="adsr" col="1" row="1" attack="0.005"/>
 *     <FX id="5" type="reverb" slot="0" mix="0.3"/>
 *   </MODULAR_PATCH>
 *
 * Module properties named after a parameter hold its value in that parameter's
 * units; missing ones take the default. Audio runs down each column in row order
 * and the columns are summed. A MOD adds `depth` (in normalised knob travel,
 * -1..1) times a source to one of the module's parameters; the source is a
 * built-in ("velocity", "macro1"...) or a modulator module ("module:<id>").
 *
 * A CABLE adds `amount` (-1..1) times another audio module's output to a
 * module's input. A source to the left of (or above) its target arrives in the
 * same sub-block; one to the right arrives one sub-block (16 samples) later,
 * which is what makes feedback possible.
 *
 * FX nodes are the stereo effects after the voices, in slot order; their MODs
 * may only use voice-independent sources (macros, mod wheel, bend). Macros take
 * their name from macroName<n>, else from what they modulate ("Cutoff").
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

namespace mt::modular
{
    namespace PatchIDs
    {
        inline const juce::Identifier MODULAR_PATCH{ "MODULAR_PATCH" };
        inline const juce::Identifier MODULE{ "MODULE" };
        inline const juce::Identifier MOD{ "MOD" };
        inline const juce::Identifier name{ "name" };
        inline const juce::Identifier voices{ "voices" };
        inline const juce::Identifier id{ "id" };
        inline const juce::Identifier type{ "type" };
        inline const juce::Identifier col{ "col" };
        inline const juce::Identifier row{ "row" };
        inline const juce::Identifier param{ "param" };
        inline const juce::Identifier source{ "source" };
        inline const juce::Identifier depth{ "depth" };
        inline const juce::Identifier FX{ "FX" };
        inline const juce::Identifier CABLE{ "CABLE" };
        inline const juce::Identifier slot{ "slot" };
        inline const juce::Identifier amount{ "amount" };
        /// Macro is 0-based: macroName1..macroName8.
        inline juce::Identifier macroName( int Macro ) { return juce::Identifier( "macroName" + juce::String( Macro + 1 ) ); }
    } // namespace PatchIDs

    constexpr int maxVoices     = 16;
    constexpr int defaultVoices = 8;
    constexpr int gridColumns   = 8; // what the editor shows; the engine does not care
    constexpr int gridRows      = 6;
    constexpr int effectSlots   = 4;

    /// A patch read out of its tree: plain data the engine can build from.
    struct PatchDesc
    {
        struct Mod
        {
            juce::String param, source;
            float        depth{ 0 };
        };

        struct Cable
        {
            juce::String source;
            float        amount{ 1 };
        };

        struct Module
        {
            int                id{ 0 };
            const ModuleSpec*  spec{ nullptr };
            int                col{ 0 }, row{ 0 };
            std::vector<float> values; // one per spec parameter, in its units
            std::vector<Mod>   mods;
            std::vector<Cable> cables;
        };

        juce::String        name;
        int                 voices{ defaultVoices };
        std::vector<Module> modules; // column, then row order; unknown types are skipped
        std::vector<Module> effects; // slot order (in `col`)

        static PatchDesc fromTree( const juce::ValueTree& Patch );

        /// Everything except knob values and depths. Two patches with the same key
        /// differ only in values, which the engine can change without rebuilding.
        juce::String structureKey() const;

        /// Index into `modules` of the module with this id, or -1.
        int indexOf( int Id ) const;
    };

    /// Builds patch trees in code (factory presets, tests).
    class PatchBuilder
    {
    public:
        explicit PatchBuilder( const juce::String& Name, int Voices = defaultVoices );

        /// Adds a module and returns its id. Values not given take their defaults.
        int add( const juce::String& Type, int Col, int Row, std::initializer_list<std::pair<const char*, float>> Values = {} );
        /// An effect in the chain after the voices; returns its id.
        int           addEffect( const juce::String& Type, int Slot, std::initializer_list<std::pair<const char*, float>> Values = {} );
        PatchBuilder& mod( int Target, const juce::String& Param, const juce::String& Source, float Depth );
        PatchBuilder& cable( int Target, int Source, float Amount = 1.f );
        PatchBuilder& macroName( int Macro, const juce::String& Name ); // 0-based

        static juce::String module( int Id ) { return "module:" + juce::String( Id ); }

        juce::ValueTree tree;

    private:
        int nextId{ 1 };
    };

    juce::StringArray getPresetNames();
    juce::ValueTree   createPreset( int Index ); // index 0 is the default patch

    /// Edits a stored patch the way the editor screen does: every call is a tree
    /// change through the undo manager (the caller begins transactions), and the
    /// program forwards it to the engine. The first edit of a factory preset marks
    /// its name ("FM Bell *") so it no longer reads as the untouched preset.
    class PatchEditor
    {
    public:
        PatchEditor( juce::ValueTree Patch, juce::UndoManager* UM ) : patch( std::move( Patch ) ), um( UM ) {}

        bool isValid() const { return patch.isValid(); }

        juce::ValueTree module( int Id ) const;
        juce::ValueTree moduleAt( int Col, int Row ) const;
        static const ModuleSpec* specOf( const juce::ValueTree& Module );

        /// Returns the new module's id, or 0 if the cell is taken or the type unknown.
        int  addModule( const juce::String& Type, int Col, int Row );
        /// Also removes the modulations it fed.
        void removeModule( int Id );
        /// Moves a module; one already in the target cell takes its old place.
        void moveModule( int Id, int Col, int Row );

        float getValue( int Id, const juce::String& Param ) const;
        void  setValue( int Id, const juce::String& Param, float Value );

        juce::ValueTree effectAt( int Slot ) const;
        /// Returns the new effect's id, or 0 if the slot is taken or the type unknown.
        int addEffect( const juce::String& Type, int Slot );

        juce::ValueTree addMod( int Target, const juce::String& Param, const juce::String& Source, float Depth );
        void            setModDepth( juce::ValueTree Mod, float Depth );
        /// Removes a MOD or a CABLE.
        void            removeMod( juce::ValueTree Mod );

        /// Audio from Source (an audio module, not Target) into Target's input.
        juce::ValueTree addCable( int Target, const juce::String& Source, float Amount );
        void            setCableAmount( juce::ValueTree Cable, float Amount );
        /// Audio modules that can feed Target, as { stored id, display name }.
        std::vector<std::pair<juce::String, juce::String>> getCableSources( int Target ) const;

        /// 0-based. The stored name, else what the macro modulates ("Cutoff",
        /// "Cutoff +2"), else "Macro 3".
        juce::String macroLabel( int Macro ) const;
        void         setMacroName( int Macro, const juce::String& Name ); // empty = derive it

        int  getVoices() const;
        void setVoices( int Voices );

        /// Modulation sources this patch offers: the built-ins, then its modulator
        /// modules, as { stored id, display name }.
        /// GlobalOnly: just the sources effects may use.
        std::vector<std::pair<juce::String, juce::String>> getSources( bool GlobalOnly = false ) const;
        /// "LFO 2": type name, numbered in grid order among modules of that type.
        juce::String moduleLabel( int Id ) const;
        juce::String sourceLabel( const juce::String& Source ) const;

    private:
        void markEdited();
        int  nextId() const;
        bool isAudioModule( int Id ) const;

        juce::ValueTree    patch;
        juce::UndoManager* um;
    };

    /// Patches saved by the user: one XML file each, in <prefs>/ModularPresets.
    struct UserPresets
    {
        static juce::File        getDirectory();
        static void              setDirectory( const juce::File& Dir ); // tests; a default File = back to prefs
        static juce::StringArray getNames();                             // sorted
        static juce::ValueTree   load( const juce::String& Name );     // invalid if missing
        /// Saves a copy called Name, replacing a user preset of that name. Refuses an
        /// empty name or a factory preset's.
        static bool save( const juce::ValueTree& Patch, const juce::String& Name );
        static bool remove( const juce::String& Name );
    };
} // namespace mt::modular
